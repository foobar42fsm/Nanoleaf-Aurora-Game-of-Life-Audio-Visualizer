/**
    Copyright 2017 Nanoleaf Ltd.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

    http:www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.

    AuroraPlugin.cpp

    Created on: Jul 27
    Author: Elliot Ford

    Description:
    Beat Detection, FFT to light source color and Panel Color calculations based on FrequncyStars by Nathan Dyck.
    Spawns a new light source at the center of a random pane when beat detected color based on fft.
    Increments age of sources every loop and removes a source either when array would be overflowed or age > lifespan.

 */


#include "AuroraPlugin.h"
#include "LayoutProcessingUtils.h"
#include "ColorUtils.h"
#include "DataManager.h"
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "Logger.h"
#include "PluginFeatures.h"


#ifdef __cplusplus
extern "C" {
#endif

    void initPlugin();
    void getPluginFrame(Frame_t* frames, int* nFrames, int* sleepTime);
    void pluginCleanup();

#ifdef __cplusplus
}
#endif

#define BASE_COLOUR_R 0 // these three settings defined the background colour; set to black
#define BASE_COLOUR_G 0
#define BASE_COLOUR_B 0
#define ADJACENT_PANEL_DISTANCE 86.599995   // hard coded distance between adjacent panels; this ideally should be autodetected
#define TRANSITION_TIME 1  // the transition time to send to panels; set to 100ms currently
#define MINIMUM_INTENSITY 0.2  // the minimum intensity of a source
#define TRIGGER_THRESHOLD 0.7 // used to calculate whether to add a source
//Light source consts
#define SPAWN_AMOUNT 1
#define LIFESPAN 1 //the max number of cycles a source will live
//Light Diffusion consts
#define TEMPO_DIVISOR 25 //default is 25
#define TEMPO_ENABLED false //determines if the tempo is taken into consideration for the diffusion
#define MININMUM_MULTIPLIER 1.5//minimum multiplier value used. Default is 1.5

// Here we store the information accociated with each light source like current
// position, velocity and colour. The information is stored in a list called sources.
typedef struct {
    float x;
    float y;
    int R;
    int G;
    int B;
    int age;
} source_t;

/** Here we store the information accociated with each frequency bin. This
 allows for tracking a degree of historical information.
 */
typedef struct {
    uint32_t latest_minimum;
    uint32_t soundPower;
    int16_t colour;
    uint32_t runningMax;
    uint32_t runningMin;
    uint32_t maximumTrigger;
    uint32_t previousPower;
    uint32_t secondPreviousPower;
} freq_bin;

static RGB_t* palettenColors = NULL; // this is our saved pointer to the colour palette
static int nColors = 0;             // the number of nColors in the palette
static LayoutData *layoutData; // this is our saved pointer to the panel layout information
static source_t* sources; // this is our array for sources
static int nSources = 0;
static freq_bin* freqBins; // this is our array for frequency bin historical information.

/**
  * @description: add a value to a running max.
  * @param: runningMax is current runningMax, valueToAdd is added to runningMax, effectiveTrail
  *         defines how many values are effectively tracked. Note this is an approximation.
  * @return: int returned as new runningMax.
  */
int addToRunningMax(int runningMax, int valueToAdd, int effectiveTrail) {
    int trail = effectiveTrail;
    if (valueToAdd > runningMax && effectiveTrail > 1) {
        trail = trail / 2;
    }
    return runningMax - ((float)runningMax / effectiveTrail) + ((float)valueToAdd / trail);
}

/**
 * @description: Initialize the plugin. Called once, when the plugin is loaded.
 * This function can be used to load the LayoutData and the colorPalette from the DataManager.
 * Any allocation, if done here, should be deallocated in the plugin cleanup function
 *
 * @param isSoundPlugin: Setting this flag will indicate that it is a sound plugin, and accordingly
 * sound data will be passed in. If not set, the plugin will be considered an effects plugin
 *
 */
void initPlugin() {
    layoutData = getLayoutData(); // grab the layout data and store a pointer to it for later use
    getColorPalette(&palettenColors, &nColors);  // grab the palette nColors and store a pointer to them for later use
    PRINTLOG("The palette has %d nColors:\n", nColors);
#define MAX_PALETTE_nColors layoutData->nPanels - 2   // if more nColors then this, we will use just the first this many
#define MAX_SOURCES layoutData->nPanels*LIFESPAN  // maxiumum sources
    PRINTLOG("MAX_SOURCES: %d\n", MAX_SOURCES);

    if(nColors > MAX_PALETTE_nColors) {
        PRINTLOG("There are too many nColors in the palette. using only the first %d\n", MAX_PALETTE_nColors);
        nColors = MAX_PALETTE_nColors;
    }
    sources = new source_t[MAX_SOURCES];
    for (int i = 0; i < nColors; i++) {
        PRINTLOG("   %d %d %d\n", palettenColors[i].R, palettenColors[i].G, palettenColors[i].B);
    }


    PRINTLOG("The layout has %d panels:\n", layoutData->nPanels);
    for (int i = 0; i < layoutData->nPanels; i++) {
        PRINTLOG("   Id: %d   X, Y: %lf, %lf\n", layoutData->panels[i].panelId,
               layoutData->panels[i].shape->getCentroid().x, layoutData->panels[i].shape->getCentroid().y);
    }


    freqBins = new freq_bin[MAX_PALETTE_nColors];
    // here we initialize our freqency bin values so that the plugin starts working reasonably well right away
    for (int i = 0; i < nColors; i++) {
        freqBins[i].latest_minimum = 0;
        freqBins[i].runningMax = 50;//Default 3
        freqBins[i].maximumTrigger = 1;//Default 1
    }
    enableFft(nColors);
    enableBeatFeatures();
}



/** Removes a light source from the list of light sources */
void removeSource(int idx)
{
    memmove(sources + idx, sources + idx + 1, sizeof(source_t) * (nSources - idx - 1));
    nSources--;
}

/** Compute cartesian distance between two points */
float distance(float x1, float y1, float x2, float y2)
{
    float dx = x2 - x1;
    float dy = y2 - y1;
    return sqrt(dx * dx + dy * dy);
}

/**
  * @description: Adds a light source to the list of light sources. The light source will have a particular colour
  * and intensity and will move at a particular speed.
*/
void addSource(int paletteIndex, float intensity)
{

    float x;
    float y;
    //int i;

    // we need at least two panels to do anything meaningful in here
    if(layoutData->nPanels < 2) {
        return;
    }
    for(int i = 0; i < SPAWN_AMOUNT; i++){
    // pick a random panel
    int n1;
    //PRINTLOG(n1);
    //int n2;
    //while(1) {
        n1 = drand48() * layoutData->nPanels;
        x = layoutData->panels[n1].shape->getCentroid().x;
        y = layoutData->panels[n1].shape->getCentroid().y;


    // decide in the colour of this light source and factor in the intensity to arrive at an RGB value
    int R = palettenColors[paletteIndex].R;
    int G = palettenColors[paletteIndex].G;
    int B = palettenColors[paletteIndex].B;
    R *= intensity;
    G *= intensity;
    B *= intensity;

    // if we have a lot of light sources already, let's bump off the oldest one
    if(nSources >= MAX_SOURCES) {
        removeSource(0);
    }
    // add all the information to the list of light sources
    sources[nSources].x = x;
    sources[nSources].y = y;
    sources[nSources].R = (int)R;
    sources[nSources].G = (int)G;
    sources[nSources].B = (int)B;
    sources[nSources].age = 0;
    //sources[nSources].alive = true;
    nSources++;
  }
}

/**
  * @description: This function will render the colour of the given single panel given
  * the positions of all the lights in the light source list.
  */
void renderPanel(Panel *panel, int *returnR, int *returnG, int *returnB)
{
    float R = BASE_COLOUR_R;
    float G = BASE_COLOUR_G;
    float B = BASE_COLOUR_B;
    int i;
    float tempo = getTempo() + 1;
    // Iterate through all the sources
    // Depending how close the source is to the panel, we take some fraction of its colour and mix it into an
    // accumulator. Newest sources have the most weight. Old sources die away until they are gone.
    for(i = 0; i < nSources; i++) {
        float d = distance(panel->shape->getCentroid().x, panel->shape->getCentroid().y, sources[i].x, sources[i].y);
        d = d / ADJACENT_PANEL_DISTANCE;
        float d2 = d*d;
        float multiplier = 0;
        if(TEMPO_ENABLED) {
          multiplier = log(tempo+1) + MININMUM_MULTIPLIER;
        } else {
          multiplier = MININMUM_MULTIPLIER;
        }
        float factor = 1.0 / (d2 * multiplier + 1.0);// determines how much of the source's colour we mix in (depends on distance)
                                                  // the formula is not based on physics, it is fudged to get a good effect
                                                  // the formula yields a number between 0 and 1
        //if(multiplier < MININMUM_MULTIPLIER) {
        //  factor = 1.0 / (d2*MININMUM_MULTIPLIER + 1.0); // Tempo is finicky, this is to make sure when Tempo = 0 the entire panel doesn't spaz
        //}

        R = R * (1.0 - factor) + sources[i].R * factor;
        G = G * (1.0 - factor) + sources[i].G * factor;
        B = B * (1.0 - factor) + sources[i].B * factor;
    }
    *returnR = (int)R;
    *returnG = (int)G;
    *returnB = (int)B;
}

/**
  * A simple algorithm to detect beats. It finds a strong signal after a period of quietness.
  * Actually, it doesn't detect just beats. For example, classical music often doesn't have
  * strong beats but it has strong instrumental sections. Those would also get detected.
  */
int16_t beat_detector(int i)
{
    int16_t beat_detected = 0;

    //Check for local maximum and if observed, add to running average
    if((freqBins[i].soundPower + (freqBins[i].runningMax / 4) < freqBins[i].previousPower) && (freqBins[i].previousPower > freqBins[i].secondPreviousPower)){
        freqBins[i].runningMax = addToRunningMax(freqBins[i].runningMax, freqBins[i].previousPower, 4);
    }

    // update latest minimum.
    if(freqBins[i].soundPower < freqBins[i].latest_minimum) {
        freqBins[i].latest_minimum = freqBins[i].soundPower;
    }
    else if(freqBins[i].latest_minimum > 0) {
        freqBins[i].latest_minimum--;
    }

    // criteria for a "beat"; value must exceed minimum plus a threshold of the runningMax.
    if(freqBins[i].soundPower > freqBins[i].latest_minimum + (freqBins[i].runningMax * TRIGGER_THRESHOLD)) {
        freqBins[i].latest_minimum = freqBins[i].soundPower;
        beat_detected = 1;
    }

    // update historical information
    freqBins[i].secondPreviousPower = freqBins[i].previousPower;
    freqBins[i].previousPower = freqBins[i].soundPower;

    return beat_detected;
}

/**
 * @description: this the 'main' function that gives a frame to the Aurora to display onto the panels
 * If the plugin is an effects plugin the soundFeature buffer will be NULL.
 * If the plugin is a sound visualization plugin, the sleepTime variable will be NULL and is not required to be
 * filled in
 * This function, if is an effects plugin, can specify the interval it is to be called at through the sleepTime variable
 * if its a sound visualization plugin, this function is called at an interval of 50ms or more.
 *
 * @param soundFeature: Carries the processed sound data from the soundModule, NULL if effects plugin
 * @param frames: a pre-allocated buffer of the Frame_t structure to fill up with RGB values to show on panels.
 * Maximum size of this buffer is equal to the number of panels
 * @param nFrames: fill with the number of frames in frames
 * @param sleepTime: specify interval after which this function is called again, NULL if sound visualization plugin
 */
void getPluginFrame(Frame_t* frames, int* nFrames, int* sleepTime) {
    int R;
    int G;
    int B;
    int i;
    uint8_t * fftBins = getFftBins();

#define SKIP_COUNT 50
    static int cnt = 0;
    if (cnt < SKIP_COUNT){
        cnt++;
        return;
    }

    // Compute the sound power (or volume) in each bin
    for(i = 0; i < nColors; i++) {
        //PRINTLOG("freq: %d max: %d power: %d\n", i, freqBins[i].runningMax, fftBins[i]);
        freqBins[i].soundPower = fftBins[i];
        uint8_t beat_detected = beat_detector(i);

        if(beat_detected) {
            if (freqBins[i].soundPower > freqBins[i].maximumTrigger) {
                freqBins[i].maximumTrigger = freqBins[i].soundPower;
            }

            float intensity = 1.0;

            //calculate an intensity ranging from minimum to 1, using log scale
            if (freqBins[i].soundPower > 1 && freqBins[i].runningMax > 1){
                intensity = ((log((float)freqBins[i].soundPower) / log((float)freqBins[i].runningMax)) * (1.0 - MINIMUM_INTENSITY)) + MINIMUM_INTENSITY;
            }

            if (intensity > 1.0) {
                intensity = 1.0;
            }

            // add a new light source for each beat detected
            addSource(i, intensity);
        }

    }


    // iterate through all the pals and render each one
    for(i = 0; i < layoutData->nPanels; i++) {
        renderPanel(&layoutData->panels[i], &R, &G, &B);
        frames[i].panelId = layoutData->panels[i].panelId;
        frames[i].r = R;
        frames[i].g = G;
        frames[i].b = B;
        frames[i].transTime = TRANSITION_TIME;
    }
    if(nSources > 0){ // just to keep the logs from filling up to much
      PRINTLOG("#sources: %d\n", nSources);
    }
    for(i = 0; i < nSources; i++) {
      if(sources[i].age == LIFESPAN) {
        removeSource(0);
      } else {
        sources[i].age++;
      }
    }

    if(TEMPO_ENABLED) {
      float tempo = getTempo();
      PRINTLOG("Tempo: %f Tempo Multi: %f\n", tempo, log(tempo + 1) + MININMUM_MULTIPLIER);
      //PRINTLOG("Energy Change: %d Energy Multi: %f\n", abs(getEnergy()-lastEnergy), (log(abs(getEnergy() - lastEnergy)+1) + MININMUM_MULTIPLIER));
    }
    //PRINTLOG("ONSET: %d\n", getIsOnset());
    // this algorithm renders every panel at every frame
    *nFrames = layoutData->nPanels;
}

/**
 * @description: called once when the plugin is being closed.
 * Do all deallocation for memory allocated in initplugin here
 */
void pluginCleanup() {
    // do deallocation here, but there is nothing to deallocate
}
