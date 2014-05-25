//*****************************************************************************
//
// dead_band.c - Example demonstrating the dead-band generator.
//
// Copyright (c) 2010-2013 Texas Instruments Incorporated.  All rights reserved.
// Software License Agreement
// 
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions
//   are met:
// 
//   Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
// 
//   Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the  
//   distribution.
// 
//   Neither the name of Texas Instruments Incorporated nor the names of
//   its contributors may be used to endorse or promote products derived
//   from this software without specific prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 
// This is part of revision 2.0.1.11577 of the Tiva Firmware Development Package.
//
//*****************************************************************************

#include <stdbool.h>
#include <stdint.h>
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "inc/hw_gpio.h"
#include "driverlib/gpio.h"
#include "driverlib/pin_map.h"
#include "driverlib/sysctl.h"
#include "driverlib/uart.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/interrupt.h"
#include "utils/uartstdio.h"

#include "driverlib/pwm.h"
#include "inc/hw_pwm.h"
#include "driverlib/qei.h"
#include "inc/hw_qei.h"
#include "driverlib/adc.h"
#include "driverlib/timer.h"

#define ARM_MATH_CM4
#define __FPU_PRESENT 1
#include "arm_math.h"

//*****************************************************************************
//
//! \addtogroup pwm_examples_list
//! <h1>PWM dead-band (dead_band)</h1>
//!
//! This example shows how to setup the PWM0 block with a dead-band generation.
//!
//! This example uses the following peripherals and I/O signals.  You must
//! review these and change as needed for your own board:
//! - GPIO Port B peripheral (for PWM pins)
//! - M0PWM0 - PB6
//! - M0PWM1 - PB7
//!
//! The following UART signals are configured only for displaying console
//! messages for this example.  These are not required for operation of the
//! PWM.
//! - UART0 peripheral
//! - GPIO Port A peripheral (for UART0 pins)
//! - UART0RX - PA0
//! - UART0TX - PA1
//!
//! This example uses the following interrupt handlers.  To use this example
//! in your own application you must add these interrupt handlers to your
//! vector table.
//! - None.
//
//*****************************************************************************


//The serial port speed
#define BAUDRATE 115200

#define MIN(X,Y) ((X) < (Y) ? (X) : (Y))
#define MAX(X,Y) ((X) > (Y) ? (X) : (Y))


// Uses floating point, only use in constant expressions!
#define CLOCKRATE 50000000
#define ClocksToUS(clocks) (((float)clocks / CLOCKRATE)*1000000)
#define USToClocks(us) ((unsigned int)((us/1000000.0f)*CLOCKRATE))
#define SToClocks(s) ((unsigned int)((float)s*CLOCKRATE))
#define FtoClocks(f) ((unsigned int)((float)CLOCKRATE/f))

// PWM period (one half of a triangle, up one slope)
static const uint32_t PWMPeriod = FtoClocks(16000);
uint32_t PWMGENS[] = { PWM_GEN_0, PWM_GEN_1, PWM_GEN_3}; //note skip of block 2!

#define PIf 3.1415926535897932384626433832795f

// Encoder conversion constants
#define VelUpdateRate 50 //Hz
static const uint32_t QEIVelocityPeriod = FtoClocks(VelUpdateRate);
#define CodewheelPPR 500
#define EdgesPerPulse 4

static const uint32_t VelCount_to_RPS_DIV = (CodewheelPPR * EdgesPerPulse) / VelUpdateRate; // Make sure this is an integer!
static const float VelCount_to_RPS_f = (float)VelUpdateRate/(CodewheelPPR * EdgesPerPulse);

// Drive Pulley dimensions
static const uint32_t RPS_to_mmPS = 5*10;

// Motor parameters
static const float lambda = 0.002238f;
static const float phaseR = 0.032f;
static const float phaseL = 23.0e-6f;


// Encoder synced
volatile bool encSync = 0;


//*****************************************************************************
//
// The error routine that is called if the driver library encounters an error.
//
//*****************************************************************************
#ifdef DEBUG
void
__error__(char *pcFilename, uint32_t ui32Line)
{
	UARTprintf("Library Error: file %s - line %d\n",pcFilename,ui32Line);
}
#endif

void PrintError(char* errorMsg, int Line){
	UARTprintf("ERROR: %s, Line %d", errorMsg, Line);
}

// Software fault
void PWMFault(){
	MAP_PWMOutputState(PWM0_BASE
	,PWM_OUT_0_BIT | PWM_OUT_1_BIT | PWM_OUT_2_BIT | PWM_OUT_3_BIT | PWM_OUT_6_BIT | PWM_OUT_7_BIT
	, false);
}


void QEIPhaseErrorHandler(){
	MAP_QEIIntClear(QEI0_BASE, QEI_INTERROR);

	PWMFault();

	//TODO: indicate error to main code
}

void QEIIdxPulseHandler(){
	MAP_QEIIntClear(QEI0_BASE, QEI_INTINDEX);

	// Turn off idx reset
	HWREG(QEI0_BASE + QEI_O_CTL) &= ~QEI_CTL_RESMODE;

	// No wrapping
	HWREG(QEI0_BASE + QEI_O_MAXPOS) = -1;

	// Mask further interrupts as we are now synced
	MAP_QEIIntDisable(QEI0_BASE, QEI_INTINDEX);

	// Inform rest of software that we are now synced
	encSync = 1;
}

void QEIHandler(){

	bool hit = 0;
	uint32_t intstatus = MAP_QEIIntStatus(QEI0_BASE, true);
	
	if (intstatus & QEI_ISC_ERROR)
	{
		hit = 1;
		QEIPhaseErrorHandler();
	}

	if (intstatus & QEI_ISC_INDEX)
	{
		hit = 1;
		QEIIdxPulseHandler();
	}

	if (!hit)
	{
		//Unimplemented interrupt!
		PWMFault();
		PrintError("Uimplemented Interrupt", __LINE__);
	}
}


//*****************************************************************************
//
// Configure the UART and its pins.  This must be called before UARTprintf().
//
//*****************************************************************************
void
ConfigureUART(void)
{
	//
	// Enable the GPIO Peripheral used by the UART.
	//
	ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);

	//
	// Enable UART0
	//
	ROM_SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);

	//
	// Configure GPIO Pins for UART mode.
	//
	ROM_GPIOPinConfigure(GPIO_PA0_U0RX);
	ROM_GPIOPinConfigure(GPIO_PA1_U0TX);
	ROM_GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);

	//
	// Use the internal 16MHz oscillator as the UART clock source.
	//
	UARTClockSourceSet(UART0_BASE, UART_CLOCK_PIOSC);

	//
	// Initialize the UART for console I/O.
	//
	UARTStdioConfig(0, BAUDRATE, 16000000);
}

// Magnitude must not be larger than sqrt(3)/2, or 0.866
void SVM(float alpha, float beta){

	static const float one_by_sqrt3 = 0.57735026919f;
	static const float two_by_sqrt3 = 1.15470053838f;

	uint32_t Sextant;

	if (beta >= 0.0f)
	{
		if (alpha >= 0.0f)
		{
			//quadrant I
			if (one_by_sqrt3 * beta > alpha)
				Sextant = 2;
			else
				Sextant = 1;

		} else {
			//quadrant II
			if (-one_by_sqrt3 * beta > alpha)
				Sextant = 3;
			else
				Sextant = 2;
		}
	} else {
		if (alpha >= 0.0f)
		{
			//quadrant IV
			if (-one_by_sqrt3 * beta > alpha)
				Sextant = 5;
			else
				Sextant = 6;
		} else {
			//quadrant III
			if (one_by_sqrt3 * beta > alpha)
				Sextant = 4;
			else
				Sextant = 5;
		}
	}

	// PWM timings
	uint32_t tA, tB, tC;

	switch (Sextant) {
	
		// sextant 1-2
		case 1:
		{
			// Vector on-times
			uint32_t t1 = (alpha - one_by_sqrt3 * beta) * PWMPeriod;
			uint32_t t2 = (two_by_sqrt3 * beta) * PWMPeriod;

			// PWM timings
			tA = (PWMPeriod - t1 - t2) / 2;
			tB = tA + t1;
			tC = tB + t2;

			break;
		}

		// sextant 2-3
		case 2:
		{
			// Vector on-times
			uint32_t t2 = (alpha + one_by_sqrt3 * beta) * PWMPeriod;
			uint32_t t3 = (-alpha + one_by_sqrt3 * beta) * PWMPeriod;

			// PWM timings
			tB = (PWMPeriod - t2 - t3) / 2;
			tA = tB + t3;
			tC = tA + t2;

			break;
		}

		// sextant 3-4
		case 3:
		{
			// Vector on-times
			uint32_t t3 = (two_by_sqrt3 * beta) * PWMPeriod;
			uint32_t t4 = (-alpha - one_by_sqrt3 * beta) * PWMPeriod;

			// PWM timings
			tB = (PWMPeriod - t3 - t4) / 2;
			tC = tB + t3;
			tA = tC + t4;

			break;
		}

		// sextant 4-5
		case 4:
		{
			// Vector on-times
			uint32_t t4 = (-alpha + one_by_sqrt3 * beta) * PWMPeriod;
			uint32_t t5 = (-two_by_sqrt3 * beta) * PWMPeriod;

			// PWM timings
			tC = (PWMPeriod - t4 - t5) / 2;
			tB = tC + t5;
			tA = tB + t4;

			break;
		}

		// sextant 5-6
		case 5:
		{
			// Vector on-times
			uint32_t t5 = (-alpha - one_by_sqrt3 * beta) * PWMPeriod;
			uint32_t t6 = (alpha - one_by_sqrt3 * beta) * PWMPeriod;

			// PWM timings
			tC = (PWMPeriod - t5 - t6) / 2;
			tA = tC + t5;
			tB = tA + t6;

			break;
		}

		// sextant 6-1
		case 6:
		{
			// Vector on-times
			uint32_t t6 = (-two_by_sqrt3 * beta) * PWMPeriod;
			uint32_t t1 = (alpha + one_by_sqrt3 * beta) * PWMPeriod;

			// PWM timings
			tA = (PWMPeriod - t6 - t1) / 2;
			tC = tA + t1;
			tB = tC + t6;

			break;
		}

	} //switch

	HWREG(PWM0_BASE + PWM_GEN_0 + PWM_O_X_CMPA) = tA;
	HWREG(PWM0_BASE + PWM_GEN_1 + PWM_O_X_CMPA) = tB;
	HWREG(PWM0_BASE + PWM_GEN_3 + PWM_O_X_CMPA) = tC; //note skip of block 2!

}



//*****************************************************************************
//
// Configure the PWM0 block with dead-band generation.  The example configures
// the PWM0 block to generate a 25% duty cycle signal on PD0 with dead-band
// generation.  This will produce a complement of PD0 on PD1 (75% duty cycle).
// The dead-band generator is set to have a 10us or 160 cycle delay
// (160cycles / 16Mhz = 10us) on the rising and falling edges of the PD0 PWM
// signal.
//
//*****************************************************************************
int
main(void)
{


	//
	// Enable lazy stacking for interrupt handlers.  This allows floating-point
	// instructions to be used within interrupt handlers, but at the expense of
	// extra stack usage.
	//
	MAP_FPULazyStackingEnable();
	//MAP_FPUStackingDisable(); // we are not using floating point in ISR's.

	MAP_IntMasterDisable(); //Turn off interrupts while we configure system.


	MAP_SysCtlClockSet(SYSCTL_SYSDIV_4 | SYSCTL_USE_PLL | SYSCTL_XTAL_16MHZ |
					   SYSCTL_OSC_MAIN);

	// Set the PWM clock to the system clock.
	MAP_SysCtlPWMClockSet(SYSCTL_PWMDIV_1);


	// Turn on all used peripherals
	MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOH); //PWM
	MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOG); //PWM
	MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF); //QEI
	MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_PWM0);
	MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_QEI0);
	MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_ADC0);
	MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_ADC1);
	MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOK); //ADC: AIN16+
	MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER0);

	// wait for units to power on
	MAP_SysCtlDelay(10);

	//
	// Initialize the UART.
	//
	ConfigureUART();



	//
	// PWM Setup
	//

	// Configure the GPIO pin muxing to select PWM functions for these pins.
	MAP_GPIOPinConfigure(GPIO_PH0_M0PWM0);
	MAP_GPIOPinConfigure(GPIO_PH1_M0PWM1);
	MAP_GPIOPinConfigure(GPIO_PH2_M0PWM2);
	MAP_GPIOPinConfigure(GPIO_PH3_M0PWM3);
	MAP_GPIOPinConfigure(GPIO_PG6_M0PWM6); //note skip of 4 and 5!
	MAP_GPIOPinConfigure(GPIO_PG7_M0PWM7);

	// Configure the GPIO pad for PWM function
	MAP_GPIOPinTypePWM(GPIO_PORTH_BASE, GPIO_PIN_0);
	MAP_GPIOPinTypePWM(GPIO_PORTH_BASE, GPIO_PIN_1);
	MAP_GPIOPinTypePWM(GPIO_PORTH_BASE, GPIO_PIN_2);
	MAP_GPIOPinTypePWM(GPIO_PORTH_BASE, GPIO_PIN_3);
	MAP_GPIOPinTypePWM(GPIO_PORTG_BASE, GPIO_PIN_6); //note skip of 4 and 5!
	MAP_GPIOPinTypePWM(GPIO_PORTG_BASE, GPIO_PIN_7);

	//override above to set 8ma drive strength (since we are using long wires)
	MAP_GPIOPadConfigSet(GPIO_PORTH_BASE, GPIO_PIN_0, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);
	MAP_GPIOPadConfigSet(GPIO_PORTH_BASE, GPIO_PIN_1, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);
	MAP_GPIOPadConfigSet(GPIO_PORTH_BASE, GPIO_PIN_2, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);
	MAP_GPIOPadConfigSet(GPIO_PORTH_BASE, GPIO_PIN_3, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);
	MAP_GPIOPadConfigSet(GPIO_PORTG_BASE, GPIO_PIN_6, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);
	MAP_GPIOPadConfigSet(GPIO_PORTG_BASE, GPIO_PIN_7, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);

	// Configure the PWM
	for (int i = 0; i < 3; ++i)
	{
		MAP_PWMGenConfigure(PWM0_BASE, PWMGENS[i],
		PWM_GEN_MODE_UP_DOWN
		| PWM_GEN_MODE_NO_SYNC //Local sync of CMP and load
		| PWM_GEN_MODE_DBG_RUN
		| PWM_GEN_MODE_GEN_NO_SYNC //No change when running
		| PWM_GEN_MODE_DB_NO_SYNC //No change when running
		| PWM_GEN_MODE_FAULT_LATCHED
		| PWM_GEN_MODE_FAULT_EXT
		);

		// Set the PWM period (one half of a triangle, up one slope)
		HWREG(PWM0_BASE + PWMGENS[i] + PWM_O_X_LOAD) = PWMPeriod;
	}

	// turn on triggering for ADC trig during inactive SVs
	PWMGenIntTrigEnable(PWM0_BASE, PWM_GEN_0, PWM_TR_CNT_ZERO | PWM_TR_CNT_LOAD);

	// Set to 50% pulse width by default
	HWREG(PWM0_BASE + PWM_GEN_0 + PWM_O_X_CMPA) = PWMPeriod / 2;
	HWREG(PWM0_BASE + PWM_GEN_0 + PWM_O_X_CMPA) = PWMPeriod / 2;
	HWREG(PWM0_BASE + PWM_GEN_0 + PWM_O_X_CMPA) = PWMPeriod / 2;


	// deadband timing. All units ns.
	// tOn* = turn on delay
	// tOff* = turn off delay
	// tRise* = Rise time
	// tFall* = Fall time
	
	//static const float tOnFET = 20;
	//static const float tRiseFET = 70;
	//static const float tOffFET = 30;
	static const float tFallFET = 40;
	//static const float tOnDriver = 160;
	//static const float toffDriver = 150;
	//static const float tRiseDriver = 100;
	static const float tFallDriver = 50;

	static const float tDelayMatchDriver = 50;
	static const float tSafety = 50;

	static const float tDeadTime = tFallFET + tFallDriver + tDelayMatchDriver + tSafety;

	// Enable dead-band generation
	for (int i = 0; i < 3; ++i)
	{
		// Enable dead-band generation
		MAP_PWMDeadBandEnable(PWM0_BASE, PWMGENS[i], USToClocks(tDeadTime/1000.0f), USToClocks(tDeadTime/1000.0f));

		// Start PWM generators
		MAP_PWMGenEnable(PWM0_BASE, PWMGENS[i]);
	}

	// Sync generators
	MAP_PWMSyncTimeBase(PWM0_BASE, PWM_GEN_0_BIT | PWM_GEN_1_BIT | PWM_GEN_3_BIT);

	// Prepare for synchronous output enable
	MAP_PWMOutputUpdateMode(PWM0_BASE
		, PWM_OUT_0_BIT | PWM_OUT_1_BIT | PWM_OUT_2_BIT | PWM_OUT_3_BIT | PWM_OUT_6_BIT | PWM_OUT_7_BIT
		, PWM_OUTPUT_MODE_SYNC_GLOBAL);

	// Queue output enable
	MAP_PWMOutputState(PWM0_BASE
		,PWM_OUT_0_BIT | PWM_OUT_1_BIT | PWM_OUT_2_BIT | PWM_OUT_3_BIT | PWM_OUT_6_BIT | PWM_OUT_7_BIT
		, true);

	// Sync apply
	MAP_PWMSyncUpdate(PWM0_BASE, PWM_GEN_0_BIT | PWM_GEN_1_BIT | PWM_GEN_3_BIT);

	// Change output mode to immediate so we can disable all outputs in code immediatly on error condition
	MAP_PWMOutputUpdateMode(PWM0_BASE
		, PWM_OUT_0_BIT | PWM_OUT_1_BIT | PWM_OUT_2_BIT | PWM_OUT_3_BIT | PWM_OUT_6_BIT | PWM_OUT_7_BIT
		, PWM_OUTPUT_MODE_NO_SYNC);



	//
	// QEI Setup
	//

	// Unlock changing purpose of PF0 (as it is a NMI)
	HWREG(GPIO_PORTF_BASE + GPIO_O_LOCK) = 0x4C4F434B; //Key
	HWREG(GPIO_PORTF_BASE + GPIO_O_CR) |= GPIO_PIN_0;
	HWREG(GPIO_PORTF_BASE + GPIO_O_LOCK) = 0; // lock when done enabling change

	MAP_GPIOPinConfigure(GPIO_PF0_PHA0);
	MAP_GPIOPinConfigure(GPIO_PF1_PHB0);
	MAP_GPIOPinConfigure(GPIO_PF4_IDX0);

	// Configure the GPIO pad for PWM function
	MAP_GPIOPinTypeQEI(GPIO_PORTF_BASE, GPIO_PIN_0);
	MAP_GPIOPinTypeQEI(GPIO_PORTF_BASE, GPIO_PIN_1);
	MAP_GPIOPinTypeQEI(GPIO_PORTF_BASE, GPIO_PIN_4);

	// Config QEI
	QEIConfigure(QEI0_BASE
		, QEI_CONFIG_CAPTURE_A_B | QEI_CONFIG_RESET_IDX | QEI_CONFIG_QUADRATURE | QEI_CONFIG_SWAP
		, 500*4 - 1);

	// Config Velocity
	MAP_QEIVelocityConfigure(QEI0_BASE, QEI_VELDIV_1, QEIVelocityPeriod);
	MAP_QEIVelocityEnable(QEI0_BASE);

	// Reg Muxing Interrupt
	QEIIntRegister(QEI0_BASE, &QEIHandler);

	// Clear above interrupts before enabling
	MAP_QEIIntClear(QEI0_BASE, QEI_INTERROR | QEI_INTINDEX);

	// Turn on above interrupts
	MAP_QEIIntEnable(QEI0_BASE, QEI_INTERROR | QEI_INTINDEX);

	// GO!
	MAP_QEIEnable(QEI0_BASE);


	//
	//ADC 0 setup (manually pumped)
	//
	MAP_GPIOPinTypeADC(GPIO_PORTK_BASE, GPIO_PIN_0);
	MAP_ADCSequenceConfigure(ADC0_BASE, 3, ADC_TRIGGER_PROCESSOR, 0);
	MAP_ADCSequenceStepConfigure(ADC0_BASE, 3, 0, ADC_CTL_CH16 | ADC_CTL_IE | ADC_CTL_END);
	MAP_ADCSequenceEnable(ADC0_BASE, 3);

	//
	//ADC 1 setup (PWM triggered)
	//
	MAP_GPIOPinTypeADC(GPIO_PORTK_BASE, GPIO_PIN_1);
	MAP_ADCSequenceConfigure(ADC1_BASE, 3, ADC_TRIGGER_PWM0, 0);
	MAP_ADCSequenceStepConfigure(ADC1_BASE, 3, 0, ADC_CTL_CH17 | ADC_CTL_IE | ADC_CTL_END);
	MAP_ADCSequenceEnable(ADC1_BASE, 3);


	//set up timer
	MAP_TimerConfigure(TIMER0_BASE, TIMER_CFG_PERIODIC);
	//set period
	MAP_TimerLoadSet(TIMER0_BASE, TIMER_A, -1);
	//GO!
	MAP_TimerEnable(TIMER0_BASE, TIMER_A);


	//Home button
	GPIODirModeSet(GPIO_PORTF_BASE, GPIO_PIN_6, GPIO_DIR_MODE_IN);
	GPIOPadConfigSet(GPIO_PORTF_BASE, GPIO_PIN_6,
                         GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);

	MAP_IntMasterEnable(); //Turn interrupts back on



	//
	// Loop forever while the PWM signals are generated.
	//

	const float amplMult = (sqrt(3)/2) * 0.99;

	// Open loop search
	float searchphase = 0;
	while(!encSync){

		float ampl = 0.02f * amplMult;
		SVM(ampl*cosf(searchphase), ampl*sinf(searchphase));

		searchphase++;
		MAP_SysCtlDelay(CLOCKRATE/100);
	}

	//while(1);

	//Lock in drive to 0 test loop
	float testphase = 0;
	//uint32_t encOffset = 133+29; //Clamp
	uint32_t encOffset = 86; //Base
	while(0)
	{
		int32_t encPhase = QEIPositionGet(QEI0_BASE) - encOffset;
		//MAP_SysCtlDelay(CLOCKRATE/4);

		float ampl = 0.02f * amplMult;
		SVM(ampl*cosf(testphase), ampl*sinf(testphase));

		const float tunegain = 0.2f * ((PIf*7.0f)/1000.0f);
		testphase -= MAX(MIN(tunegain * encPhase, 100*tunegain), -100*tunegain);

		UARTprintf("%d\t%d\n", encPhase, (int32_t)(testphase*(1000.0f/(3.14159f*7.0f))));

		MAP_SysCtlDelay(CLOCKRATE/100);
	}

	//while(1);


	//full speed ahead ;D
	uint32_t adcval = 0;
	uint32_t t_latency = 0;
	uint32_t t_period = 0;

	float speedKP = 0.2f; // A/(rad/s)
	float maxspeed = 0.2f; // m/s

	float posKP = 4.0f; // m/s per m
	float posSetpoint = 0.35f; //m from left (basline at encSync)

	float goHomeOmega = -30.0f;
	bool homed = 0;
	int32_t homeencPos = 0;

	float BusVoltage = 12.0f;
	float maxcurrent = 20.0f;
	float maxmod = 0.4f;
	
	ADCProcessorTrigger(ADC0_BASE, 3);
	while(1){

		uint32_t t_start = TimerValueGet(TIMER0_BASE, TIMER_A); //note counts backwards

		int32_t encPos = QEIPositionGet(QEI0_BASE) - encOffset;
		int32_t encPhase = encPos % 2000;
		float rotorPhase = (float)encPhase * ((3.14159f*7.0f)/1000.0f);

		//Latency compensation
		uint32_t t_compensation = t_latency + t_period/2 + PWMPeriod; //TODO: add LR
		int32_t ivel = QEIDirectionGet(QEI0_BASE) * (int32_t)QEIVelocityGet(QEI0_BASE);
		float omega = 7.0f * 2.0f * PIf * VelCount_to_RPS_f * (float)ivel;
		float compPhase = omega * (float)t_compensation/CLOCKRATE;
		rotorPhase += compPhase;

		//Manual phase injection
		float phaseoffset_man = 0.1f * ((float)adcval/4096.0f);
		rotorPhase += phaseoffset_man;

		//Position control
		float currpos = (encPos - homeencPos) * (1.0f/2000.0f * 10.0f * 5.0f/1000.0f); //m
		float velSetpoint = MIN(MAX(posKP * (posSetpoint - currpos), -maxspeed), maxspeed);
		float omegaSetpoint = (7.0f * 2.0f * PIf * 1.0f/(10.0f*0.005f)) * velSetpoint;

		//homing or position control
		if (!homed && GPIOPinRead(GPIO_PORTF_BASE, GPIO_PIN_6))
		{
			homeencPos = encPos;
			homed = 1;
		}

		//Current control
		//TODO useless omega scaling, do direct m/s to A
		float eOmega;
		if (homed)
			eOmega = omegaSetpoint - omega;
		else
			eOmega = goHomeOmega - omega;

		float Iq = speedKP * eOmega;

		Iq = MIN(MAX(Iq, -maxcurrent), maxcurrent);

		float Vd = -Iq * phaseL * omega;
		float Vq = lambda * omega + Iq * phaseR;

		float VtoModulation = 1.0f / ((2.0f/3.0f) * BusVoltage);

		Vd *= VtoModulation;
		Vq *= VtoModulation;

		Vd = MIN(MAX(Vd, -maxmod*amplMult), maxmod*amplMult);
		Vq = MIN(MAX(Vq, -maxmod*amplMult), maxmod*amplMult);

		//float ampl = 0.1f;
		//float ampl = ((0.4f * (float)adcval/4096.0f) + 0.01) * amplMult;
		// float Vd = 0;
		// float Vq = ampl;

		// Vd *= amplMult; //map 1.0 to max modulation
		// Vq *= amplMult;

		SVM(Vd*cosf(rotorPhase) - Vq*sinf(rotorPhase), Vd*sinf(rotorPhase) + Vq*cosf(rotorPhase));
		uint32_t t_end = TimerValueGet(TIMER0_BASE, TIMER_A); //note counts backwards

		//Debug
		int32_t encPhasePSVM = QEIPositionGet(QEI0_BASE) - encOffset;
		encPhasePSVM %= 2000;

		if (ADCIntStatus(ADC0_BASE, 3, false))
		{
			ADCIntClear(ADC0_BASE, 3);
			ADCSequenceDataGet(ADC0_BASE, 3, &adcval);
			ADCProcessorTrigger(ADC0_BASE, 3);
		}

		if (ADCIntStatus(ADC1_BASE, 3, false))
		{
			ADCIntClear(ADC1_BASE, 3);
			uint32_t temp;
			ADCSequenceDataGet(ADC1_BASE, 3, &temp);
			BusVoltage = temp * ((4.0f * 3.3f) / (float)(1<<12));
		}

		t_latency = t_start-t_end;
		static uint32_t t_end_last = 0;
		t_period = t_end_last-t_end;
		t_end_last = t_end;

		static float manphasefiltstate = 0;
		manphasefiltstate += (phaseoffset_man - manphasefiltstate)*0.001;

		static uint32_t d = 0;
		if (++d == CLOCKRATE/10000)
		{
			d = 0;
			
			//float rps = (float)ivel / VelCount_to_RPS_DIV;
			
			//UARTprintf("%d\n", (uint32_t)(1000*rps));
			//UARTprintf("%d\n", (int32_t)(phaseoffset_man*1000));
			//UARTprintf("%d\n", encPhasePSVM-encPhase);
			//UARTprintf("%d\t%d\n",t_latency ,t_period );
			//UARTprintf("%d\t%d\n",(int32_t)(manphasefiltstate*1000), (int32_t)(compPhase*1000) );
			UARTprintf("%d\t%d\n", (int32_t)(Iq*1000), (int32_t)(BusVoltage*1000));
		}
	}
}
