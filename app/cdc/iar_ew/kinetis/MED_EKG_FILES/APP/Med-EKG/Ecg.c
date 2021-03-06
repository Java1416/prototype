#include "ECG.h"

//public variables
UINT8 Ecg_HeartRate = 0;
UINT8 EcgDataBuffer[ECG_DATA_BUFFER_LENGTH];


//private
static UINT8 QRSFound = 0;
static UINT16 RealTimeHeartRate;
static UINT16 EcgHeartRateSum;
static UINT16 EcgHeartRateArray[ECG_ARRAY_LENGTH];
static UINT16 CopyOfEcgHeartRateArray[ECG_ARRAY_LENGTH];
static UINT8 EcgCurrentArrayPosition = 0;
static UINT16 EcgFirstSample = 0;
static UINT16 EcgSecondSample = 0;
static UINT16 EcgThirdSample = 0;
static UINT16 samplesBetweenPulses = 0;
static UINT8 HeartBeatOcurred;
static UINT8 IsEcgSignalReady;


/* Private functions */
static void StateIdle(void);
static void StateMeasuring(void);
static void TimerSampleAdc_Event(void);

static void PerformControlAlgorithm(void);
static void SendGraphDataToPc(void);
static void ClearAllVariables(void);
static void CalculateHeartRateMedian(void);


/* Main state machine */
void (*const EcgStateMachine[]) (void) =
{
	StateIdle,
	StateMeasuring
};

/* Private Macros */

typedef enum
{
	STATE_IDLE,
	STATE_MEASURING
} EcgStates_e;

/* Private variables */
static MovingAverage_uint16_t FeedbackSignal, EcgSignal;
static UINT8 EcgActualState = STATE_IDLE;
static TIMER_OBJECT TimerEcgSampleAdc;
static UINT8 TimerEcgSampleAdcIndex;
static UINT8 EcgActualEvent = EVENT_ECG_NONE;

static UINT8 IsHeartRateMode = FALSE;
static UINT8 IsNewEcgSampleReady = FALSE;

static UINT16 PulseDetectedWatchDog = 0;

/*Francisco Variables */
static UINT8 n = 100;
static UINT16 PwmValue = 0;

/*************************************************************/



/* Private function definitions */

static void StateIdle(void)
{
	//do nothing
}



/*------------------------ definitions and variables ---------------------*/
#define SAMPLES_NUMBER 15


#define CENTER_LOW     32768-3000   //1.45V  
#define CENTER_HI      32768+3000   //1.75V   

#define LOW_LIMIT_1    32768-12000  //1.0V
#define HI_LIMIT_1     32768+12000  //2.2V
#define LOW_LIMIT_2    32768-22000  //0.5V
#define HI_LIMIT_2     32768+22000  //2.7V

#define FEW_CORRECTION   19   //0.015V -> with 3.3 gain, it adds about 0.05V     
#define MORE_CORRECTION  38   //0.030V -> with 3.3 gain, it adds about 0.10V  
#define MUCH_CORRECTION  76   //0.060V -> with 3.3 gain, it adds about 0.20V

UINT8 SampleCounter = 0, i_index, j_index;
UINT16 median_array[SAMPLES_NUMBER], temp_var, median_val;

/*------------------------------------------------------------------------*/

static void PerformControlAlgorithm(void)
{
	if ( SampleCounter < SAMPLES_NUMBER)
	{
		median_array[SampleCounter] = FeedbackSignal.Result;
		SampleCounter++;
	}
	else
	{
		SampleCounter = 0;

		//ordering the data

		for (i_index = 0; i_index <= SAMPLES_NUMBER - 2; i_index++)
		{
			for (j_index = i_index + 1; j_index <= SAMPLES_NUMBER - 1; j_index++)
			{
				if (median_array[i_index] > median_array[j_index])
				{
					temp_var = median_array[i_index];
					median_array[i_index] = median_array[j_index];
					median_array[j_index] = temp_var;
				}
			}
		}

		//obtaining the median
		median_val = median_array[SAMPLES_NUMBER/2];

		//compensate acording to the value of the median
		if ( (median_val < CENTER_LOW) || (median_val > CENTER_HI) )
		{

			if ( (median_val < CENTER_LOW)        &&
			        (median_val > LOW_LIMIT_1)       &&
			        (DACDAT0 < (4094 - FEW_CORRECTION)) )
			{
				DACDAT0 += FEW_CORRECTION;
			}

			if ( (median_val > CENTER_HI)      &&
			        (median_val < HI_LIMIT_1)     &&
			        (DACDAT0 > (FEW_CORRECTION + 1)) )
			{
				DACDAT0 -= FEW_CORRECTION;
			}

			if ( (median_val < LOW_LIMIT_1)        &&
			        (median_val > LOW_LIMIT_2)        &&
			        (DACDAT0 < (4094 - MORE_CORRECTION)) )
			{
				DACDAT0 += MORE_CORRECTION;
			}

			if ( (median_val > HI_LIMIT_1)      &&
			        (median_val < HI_LIMIT_2)      &&
			        (DACDAT0 > (MORE_CORRECTION + 1)) )
			{
				DACDAT0 -= MORE_CORRECTION;
			}

			if ( (median_val < LOW_LIMIT_2)        &&
			        (DACDAT0 < (4094 - MUCH_CORRECTION)) )
			{
				DACDAT0 += MUCH_CORRECTION;
			}

			if ( (median_val > HI_LIMIT_2)      &&
			        (DACDAT0 > (MUCH_CORRECTION + 1)) )
			{
				DACDAT0 -= MUCH_CORRECTION;
			}

		}

		//if the median are in the range, the DAC provides 1.6V
		else
		{
			DACDAT0 = 2047; //1.6V output
		}

	}
}



static void StateMeasuring(void)
{

	if (IsNewEcgSampleReady) //the timer is reading periodically
	{ 

		{
			UINT16 feedbackSignalRaw, ecgSignalRaw;
			IsNewEcgSampleReady = FALSE;


			//read ADC signals and average values
			feedbackSignalRaw = ADC1_Read16b(ADC_CHANNEL_FEEDBACK_SIGNAL);
			ecgSignalRaw = ADC0_Read16b(ADC_CHANNEL_ECG_SIGNAL);

			MovingAverage_PushNewValue16b(&FeedbackSignal, feedbackSignalRaw);
			MovingAverage_PushNewValue16b(&EcgSignal, ecgSignalRaw);

			TimerEcgSampleAdcIndex = AddTimerQ(&TimerEcgSampleAdc);
		}

		PerformControlAlgorithm();
                
                
		samplesBetweenPulses++; 		//increment sample sampleCounter between pulses
		PulseDetectedWatchDog++;		//this variable increments every ECG_SAMPLE_PERIOD (2ms)
		
		if (PulseDetectedWatchDog > MAX_TIME_WITHOUT_PULSES)
		{
			Ecg_HeartRate = 0;		//set HR = 0
		}


		EcgFirstSample = EcgSecondSample;
		EcgSecondSample = EcgThirdSample;
		EcgThirdSample = EcgSignal.Result;


		//If the slope of the signal is big, there is a peak

		if ((EcgThirdSample < EcgFirstSample) && ((EcgFirstSample - EcgThirdSample) > HR_SLOPE_THRESHOLD))
		{
			//Peak detected
			QRSFound++;
			PulseDetectedWatchDog = 0;

			if (samplesBetweenPulses > 75) 		//check if current peak is not to close to previous peak (Max HR = 200)
			{
				IsEcgSignalReady = TRUE;
				HeartBeatOcurred = TRUE;


				if (QRSFound > 4)  					// Find 4 Pulses Before Considering Signal is Ready
				{
					UINT8 i;

					//4 pulses has been found
					RealTimeHeartRate = 60000 / (samplesBetweenPulses * ECG_SAMPLING_PERIOD); 	//Calculate HR

					//Shift samples of FIFO

					for (i = OLDEST_ELEMENT; i > NEWEST_ELEMENT; i--)
					{
						EcgHeartRateArray[i] = EcgHeartRateArray[i-1];
					}

					//insert new sample into FIFO
					EcgHeartRateArray[NEWEST_ELEMENT] = RealTimeHeartRate;

					//Call the function to calculate heart rate
					CalculateHeartRateMedian();

				}
				else
				{
					//less than 4 QRs
				}
			}

			else if (
			    ((samplesBetweenPulses > 10) && (samplesBetweenPulses < 500)) ||
			    ((EcgFirstSample - EcgSecondSample) > 400) ||
			    (EcgSignal.Result > 1750)
			)
			{
				//time between peaks is shorter
				IsEcgSignalReady = FALSE;
			}

			samplesBetweenPulses = 0;
		}

		if (!IsHeartRateMode)
		{
			SendGraphDataToPc();
		}
	}
}


#define ZERO_SIGNAL		0x00


static void SendGraphDataToPc(void)
{
	static UINT8 actualPosition = 0;

	//store data and send it when the buffer is full

	if (actualPosition < ECG_DATA_BUFFER_LENGTH)
	{
		//if (IsEcgSignalReady)
		{
			//we need to convert unsigned values to signed values to display them on GUI
			INT16 signedEcgSignal = (INT16)(EcgSignal.Result - 0x8000);

			EcgDataBuffer[actualPosition++] = (UINT8)(signedEcgSignal >> 8); 		//higher byte
			EcgDataBuffer[actualPosition++] = (UINT8)(signedEcgSignal & 0x00FF); 	//lower byte
		}/*

		else
		{
			EcgDataBuffer[actualPosition++] = (UINT8)(ZERO_SIGNAL >> 8); 		//higher byte
			EcgDataBuffer[actualPosition++] = (UINT8)(ZERO_SIGNAL & 0x00FF); 	//lower byte
		}*/
	}
	else
	{
		//buffer is full, call the EVENT_ECG_DATA_READY event
		EcgActualEvent = EVENT_ECG_DIAGNOSTIC_MODE_NEW_DATA_READY;
		actualPosition = 0;
	}
}


static void TimerSampleAdc_Event(void)
{
	IsNewEcgSampleReady = TRUE;
}




/*******************************
* Public functions
*********************************/

/* call this only once at the beginning of the application */
void Ecg_Init(void)
{
	TimerEcgSampleAdc.msCount = ECG_SAMPLING_PERIOD;
	TimerEcgSampleAdc.pfnTimerCallback = TimerSampleAdc_Event;
}


UINT8 Ecg_DiagnosticModeStartMeasurement(void)
{
	UINT8 status = FALSE;

	if (EcgActualState == STATE_IDLE)
	{
		ADC0_Init16b();
                ADC1_Init16b();
                //1 << ADC_CHANNEL_FEEDBACK_SIGNAL |
		            //1 << ADC_CHANNEL_ECG_SIGNAL);

		//TPM1_Init();	//TPM is not used in MM version, DAC is used instead

		//TO DO:

		//DAC_Init();
		//OpAmps_Init();
				
		ClearAllVariables();

		EcgActualState = STATE_MEASURING;
		IsHeartRateMode = FALSE;

		//start timer to sample ADC
		TimerEcgSampleAdcIndex = AddTimerQ(&TimerEcgSampleAdc);
		status = TRUE;
	}
	else
	{
		status = FALSE;
	}

	return status;
}


static void ClearAllVariables(void)
{
	UINT8 i;

	Ecg_HeartRate = 0;
	QRSFound = 0;
	RealTimeHeartRate = 0;
	EcgHeartRateSum = 0;
	EcgCurrentArrayPosition = 0;
	EcgFirstSample = 0;
	EcgSecondSample = 0;
	EcgThirdSample = 0;
	samplesBetweenPulses = 0;
	HeartBeatOcurred = 0;
	IsEcgSignalReady = 0;
	EcgCurrentArrayPosition = 0;
	PulseDetectedWatchDog = 0;
	
	//clear arrays
	for (i = 0; i < ECG_ARRAY_LENGTH; i++)
	{
		EcgHeartRateArray[i] = 0;
	}

}




void Ecg_DiagnosticModeStopMeasurement(void)
{
	RemoveTimerQ(TimerEcgSampleAdcIndex);
	EcgActualState = STATE_IDLE;
}



void Ecg_PeriodicTask(void)
{
	/* State machine handler */
	EcgStateMachine[EcgActualState]();

	/* Event handler */

	if (EcgActualEvent != EVENT_ECG_NONE)
	{
		if (Ecg_Events[EcgActualEvent] != NULL)
		{
			Ecg_Events[EcgActualEvent]();	//execute registered event
			EcgActualEvent = EVENT_ECG_NONE;
		}
	}

}








static void CalculateHeartRateMedian(void)
//order HearRate values in array and average samples in the middle
{
	UINT8 startIndex;
	UINT8 smallestIndex;
	UINT8 currentIndex;
	UINT8 tempStoreValue;
	UINT8 i;
	static UINT16 Ecg_HeartRate_MedianSum = 0;
	static UINT8 median_counter = 0;

	//Create a copy of the arrays

	for (i = 0; i < ECG_ARRAY_LENGTH; i++)
	{
		CopyOfEcgHeartRateArray[i] = EcgHeartRateArray[i];
	}

	// Order array values in ascending order
	for (startIndex = 0; startIndex < ECG_ARRAY_LENGTH; startIndex++)
	{
		smallestIndex = startIndex;

		for (currentIndex = startIndex + 1; currentIndex < ECG_ARRAY_LENGTH; currentIndex++)
		{
			if (CopyOfEcgHeartRateArray[currentIndex] < CopyOfEcgHeartRateArray[smallestIndex])
			{
				smallestIndex = currentIndex;
			}
		}

		tempStoreValue = (UINT8) CopyOfEcgHeartRateArray[startIndex];

		CopyOfEcgHeartRateArray[startIndex] = CopyOfEcgHeartRateArray[smallestIndex];
		CopyOfEcgHeartRateArray[smallestIndex] = tempStoreValue;
	}

	//Obtaining the median
	if (median_counter < 4)
	{
		Ecg_HeartRate_MedianSum += CopyOfEcgHeartRateArray[ECG_ARRAY_LENGTH/2];
		median_counter++;
	}
	else
	{
		Ecg_HeartRate = (UINT8)(Ecg_HeartRate_MedianSum / 4);
		median_counter = 0;
		Ecg_HeartRate_MedianSum = 0;
	}
}
