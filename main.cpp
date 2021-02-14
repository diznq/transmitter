#include "mbed.h"

Serial PC(USBTX,USBRX);

DigitalOut red(LED_RED);
DigitalOut green(LED_GREEN);
DigitalOut blue(LED_BLUE);

AnalogOut dac(DAC0_OUT);

/* Execute code at given address */
#define exec(op) ((void(*)()) ((uintptr_t) op | 1))()

#define SINE 0
#define SQUARE 1
#define WAVEFORM SQUARE

#define MAX_OPCODES 80      /* Maximum length of instructions buffer */
#define LED_ON 0
#define LED_OFF 1

#define MEASURING 2
#define TESTING 1
#define BROADCASTING 0

static uint16_t opcodes[MAX_OPCODES];   /* Instructions buffer */

/**
 * Initialize ADC
 */
inline void init_adc(){
    SIM->SCGC6 |= SIM_SCGC6_ADC0_MASK;		/* Enable ADC0 Clock */
	ADC0->CFG1 |= ADC_CFG1_MODE(3);			/* 16bit ADC */
	ADC0->SC1[0] |= ADC_SC1_ADCH(31);		/* 11111 = disabled mode */
}

/**
 * Transmit single period
 */
inline void transmit(const int value){
#if WAVEFORM == SINE
    dac.write_u16(0);
    exec(opcodes);
    dac.write_u16(value >> 1);
    exec(opcodes);
    dac.write_u16(value);
    exec(opcodes);
    dac.write_u16(value >> 1);
    exec(opcodes);
#else
    dac.write_u16(value);
    exec(opcodes);
    dac.write_u16(0);
    exec(opcodes);
#endif
}

int main(){
    Timer timer;    /* We will use this to measure time in microseconds */
    timer.start();  /* Start measuring now */
    PC.baud(115200);    /* Set-up serial port */

    /* Initialize our instructions array to be all NOPs */
    for(int i=0; i<MAX_OPCODES;i++){ 
        opcodes[i] = 0xBF00;    
    }

    /**
     * In order to perform sleep in nanoseconds, we cannot use for loop.
     * Instead we create an array of NOPs (BF00) and set RET (BR LX | 4770)
     * at the point, where we want to end sleep.
     * Thus we can accurately execute `n` instructions.
     * For each possible combination we will measure period and save it to
     * following array
     */ 
    unsigned int measurements[MAX_OPCODES];
    
    unsigned int
        index = 0,              /* Our pointer to where BR LX is currently located at */
        read_state = 0,         /* ADC state, 0 = begin read, 1 = reading step 1, 2 = reading step 2, 3 = read finished */
        ready_state = MEASURING,/* Ready state */
        periods = 100000,       /* Amount of periods per one sample */
        adc_value = 0,          /* Last read ADC value (16-bit integer) */
        sample_rate = 22050,     /* Sample rate */
        start = 0,              /* Start time of measurement */
        end = 0,                /* End time of measurement */
        best_index = 0,         /* Best pointer yet found */
        best_frequency = 0;     /* Best frequency we are able to match yet */

    opcodes[index] = 0x4770;    /* Set initial BR LX */

    float desired = 558000.0f,  /* Desired frequency */
          best_diff = desired,  /* Best delta we've found yet ( |Measured - Desired| ) */
          freq = 0.0f,          /* Current broadcast frequency */
          diff = 0.0f;          /* Current delta */
    unsigned int measure_limit = MAX_OPCODES - 1;    /* Pointer limit, in order to not waste time on values like 5000 (takes 20s to test) */

    /**
     * List of frequencies we will try to match 
     * 
     * I just listed stuff that my radio at home is able to handle :D
     * Nothing special about these numbers, you can change them to anything else
     */
    float frequencies[] = {
        531000, 540000, 549000, 558000, 567000, 576000, 585000, 594000, 
        603000, 612000, 621000, 630000, 639000, 648000, 651000, 666000, 
        675000, 684000, 693000, 702000, 711000, 720000, 729000, 738000, 
        747000, 756000, 765000, 774000, 783000, 792000, 801000, 810000, 
        819000, 828000, 837000, 846000, 855000, 864000, 873000, 882000, 
        891000, 900000, 909000, 918000, 927000, 936000, 945000, 954000, 
        963000, 972000, 981000, 981000, 989000, 990000, 999000, 1008000, 
        1017000, 1026000, 1035000, 1044000, 1053000, 1062000, 1071000, 1080000,
        1089000, 1098000, 1107000, 1115000, 1116000, 1125000, 1134000, 1143000,
        1152000, 1161000, 1170000, 1179000, 1188000, 1197000, 1206000, 1215000,
    };

    unsigned int frequencies_n = sizeof(frequencies) / sizeof(float);

    /* Since we are broadcasting in USB mode, we need to shift left by SampleRate / 2 */
    /* also to speed up computation, we can convert it to periods in us */
    for(unsigned int i = 0; i < frequencies_n; i++){
        frequencies[i] -= sample_rate / 2.0f;
    }

    init_adc(); /* Initialize ADC */

    /* Switch measuring state signalisation (red LED) */
    red = LED_ON, green = LED_OFF, blue = LED_OFF;

    while(true){
        /**
         * ADC state machine
         * Since we cannot use blocking functions like analogin_read(),
         * we need to read value ourselves by creating simple state machine
         * that writes and reads correct values to / from ADC
         */
        switch(read_state){
            case 0:
                ADC0->SC1[0] = 0x0C & ADC_SC1_ADCH_MASK;    /* Enable ADC12 ( pin A0 ) */
                read_state = 1;                             /* Move to next state */
                break;
            case 1:
                if(!(ADC0->SC2 & ADC_SC2_ADACT_MASK))       /* Until SC2 has ADC_SC2_ADACT_MASK flag enabled, we wait */
                    read_state = 2;                         /* Then we move to state 2 */
                break;
            case 2:
                if( (ADC0->SC1[0] & ADC_SC1_COCO_MASK) == ADC_SC1_COCO_MASK )   /* If reading value is finished */
                    read_state = 3;                                             /* Then we move to final state 3 */
                break;
            case 3:
                adc_value = ADC0->R[0]; /* ADC read is finished, so we preserve value for further use */
                read_state = 0;         /* And we loop to beginning */
                break;
        }

        /**
         * Broadcast state machine
         */
        switch(ready_state){
            /**
             * In state 2, we actively search for best frequency we can match
             */
            case MEASURING:
                /**
                 * We do so by measuring time it takes to execute 100 000 periods at given BR LX location 
                 */
                start = timer.read_us();
                for(unsigned int i=periods; i; i--){
                    transmit(adc_value);
                }
                end = timer.read_us();
                /**
                 * We save this measurement, and move on to next BR LX location
                 */
                measurements[index] = end - start;
                opcodes[index] = 0xBF00;
                opcodes[++index] = 0x4770;
                /**
                 * Once we tried all BR LX locations, it's time to evaluate which one matched desired frequencies the best
                 */
                if(index == measure_limit){
                    /* Inform user that we are evaluating measurements (blue LED) */
                    red = LED_OFF; green = LED_OFF; blue = LED_ON;
                    for(unsigned int i=0; i<measure_limit - 1; i++){
                        for(unsigned int j=0; j<frequencies_n; j++){
                            /**
                             * We evaluate each measurement and each desired frequency and 
                             * calculate delta = |measured - desired|
                             * If delta is smaller that best delta we've found yet,
                             * we update best pointer, frequency and best delta
                             */
                            freq = periods * 1000000.0f / measurements[i];
                            diff = abs(freq - frequencies[j]);
                            if(diff < best_diff){
                                best_diff = diff;
                                best_frequency = j;
                                best_index = i;
                            }
                        }
                    }
                    /**
                     * After evaluation is done, it's time to move to testing state
                     * So first, let's set BR LX to where it belongs to and inform user
                     * we are testing now (cyan LED)
                     */
                    freq = periods * 1000000.0f / measurements[index];
                    periods = 250000;
                    opcodes[index] = 0xBF00;
                    opcodes[index = best_index] = 0x4770;
                    desired = frequencies[best_frequency];
                    ready_state = TESTING;
                    red = LED_OFF; green = LED_ON; blue = LED_ON;
                }
                break;
            case TESTING:
                /**
                 * Basically do the same as before, but with period = 1 000
                 */
                start = timer.read_us();
                for(unsigned int i=periods; i; i--){
                    transmit(adc_value);
                }
                end = timer.read_us();
                freq = periods * 1000000.0f / (end - start);
                periods = (int)(0.5f * freq / sample_rate);
                PC.printf("Broadcast: measured=%f, desired=%f (%f), error=%f, final periods=%d\n", freq, desired, desired + sample_rate / 2, freq - desired, periods);
                /**
                 * In order to broadcast, we need to set period to something sensible
                 * Since we are broadcasting on frequency F and sample rate SR is smaller than SR
                 * it means need to repeat same sample several times to broadcast it
                 * We calculate this amount as 0.5 Frequency / SampleRate.
                 * Constant 0.5 comes from fact that period consists of two parts: High and Low
                 */
                ready_state = BROADCASTING;
                /* Inform user that we are broadcasting now (green LED) */
                red = LED_OFF; green = LED_ON; blue = LED_OFF;
                break;
            case BROADCASTING:
                /* Broadcast ADC value that's been read */
                for(unsigned int i=periods; i; i--){
                    transmit(adc_value);
                }
                break;
        }
    }
}