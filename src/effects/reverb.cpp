#include "reverb.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

// ============================================================================
// MVerb — Martin Eastwood
// Copyright (c) 2010 Martin Eastwood
// Distributed under the GNU General Public License v3 or later.
// Source: https://github.com/martineastwood/mverb/blob/master/MVerb.h
// ============================================================================

// forward declarations
template<typename T, int maxLength> class Allpass;
template<typename T, int maxLength> class StaticAllpassFourTap;
template<typename T, int maxLength> class StaticDelayLine;
template<typename T, int maxLength> class StaticDelayLineFourTap;
template<typename T, int maxLength> class StaticDelayLineEightTap;
template<typename T, int OverSampleCount> class StateVariable;

template<typename T>
class MVerb
{
private:
    Allpass<T, 96000> allpass[4] = {};
    StaticAllpassFourTap<T, 96000> allpassFourTap[4] = {};
    StateVariable<T,4> bandwidthFilter[2] = {};
    StateVariable<T,4> damping[2] = {};
    StaticDelayLine<T, 96000> predelay = {};
    StaticDelayLineFourTap<T, 96000> staticDelayLine[4] = {};
    StaticDelayLineEightTap<T, 96000> earlyReflectionsDelayLine[2] = {};
    T SampleRate = {};
    T DampingFreq = {};
    T Density1 = {};
    T Density2 = {};
    T BandwidthFreq = {};
    T PreDelayTime = {};
    T Decay = {};
    T Gain = {};
    T Mix = {};
    T EarlyMix = {};
    T Size = {};

    T MixSmooth = {};
    T EarlyLateSmooth = {};
    T BandwidthSmooth = {};
    T DampingSmooth = {};
    T PredelaySmooth = {};
    T SizeSmooth = {};
    T DensitySmooth = {};
    T DecaySmooth = {};

    T PreviousLeftTank = {};
    T PreviousRightTank = {};

    int ControlRate = 0;
    int ControlRateCounter = 0;

public:
    enum
    {
        DAMPINGFREQ=0,
        DENSITY,
        BANDWIDTHFREQ,
        DECAY,
        PREDELAY,
        SIZE,
        GAIN,
        MIX,
        EARLYMIX,
        NUM_PARAMS
    };

    MVerb(){
        DampingFreq = 0.9;
        BandwidthFreq = 0.9;
        SampleRate = 44100.;
        Decay = 0.5;
        Gain = 1.;
        Mix = 1.;
        Size = 1.;
        EarlyMix = 1.;
        PreviousLeftTank = 0.;
        PreviousRightTank = 0.;
        PreDelayTime = 100 * (SampleRate / 1000);
        MixSmooth = EarlyLateSmooth = BandwidthSmooth = DampingSmooth =
            PredelaySmooth = SizeSmooth = DecaySmooth = DensitySmooth = 0.;
        ControlRate = static_cast<int>(SampleRate / 1000);
        ControlRateCounter = 0;
        reset();
    }

    ~MVerb(){}

    void process(T **inputs, T **outputs, int sampleFrames){
        T OneOverSampleFrames = static_cast<T>(1. / sampleFrames);
        T MixDelta           = (Mix - MixSmooth) * OneOverSampleFrames;
        T EarlyLateDelta     = (EarlyMix - EarlyLateSmooth) * OneOverSampleFrames;
        T BandwidthDelta     = static_cast<T>((((BandwidthFreq * 18400.) + 100.) - BandwidthSmooth) * OneOverSampleFrames);
        T DampingDelta       = static_cast<T>((((DampingFreq * 18400.) + 100.) - DampingSmooth) * OneOverSampleFrames);
        T PredelayDelta      = static_cast<T>(((PreDelayTime * 200 * (SampleRate / 1000)) - PredelaySmooth) * OneOverSampleFrames);
        T SizeDelta          = static_cast<T>((Size - SizeSmooth) * OneOverSampleFrames);
        T DecayDelta         = static_cast<T>((((0.7995f * Decay) + 0.005) - DecaySmooth) * OneOverSampleFrames);
        T DensityDelta       = static_cast<T>((((0.7995f * Density1) + 0.005) - DensitySmooth) * OneOverSampleFrames);

        for(int i = 0; i < sampleFrames; ++i){
            T left  = inputs[0][i];
            T right = inputs[1][i];

            MixSmooth        += MixDelta;
            EarlyLateSmooth  += EarlyLateDelta;
            BandwidthSmooth  += BandwidthDelta;
            DampingSmooth    += DampingDelta;
            PredelaySmooth   += PredelayDelta;
            SizeSmooth       += SizeDelta;
            DecaySmooth      += DecayDelta;
            DensitySmooth    += DensityDelta;

            if (ControlRateCounter >= ControlRate){
                ControlRateCounter = 0;
                bandwidthFilter[0].Frequency(BandwidthSmooth);
                bandwidthFilter[1].Frequency(BandwidthSmooth);
                damping[0].Frequency(DampingSmooth);
                damping[1].Frequency(DampingSmooth);
            }
            ++ControlRateCounter;

            predelay.SetLength(static_cast<int>(PredelaySmooth));
            Density2 = static_cast<T>(DecaySmooth + 0.15);
            if (Density2 > 0.5)  Density2 = 0.5;
            if (Density2 < 0.25) Density2 = 0.25;

            allpassFourTap[1].SetFeedback(Density2);
            allpassFourTap[3].SetFeedback(Density2);
            allpassFourTap[0].SetFeedback(DensitySmooth);
            allpassFourTap[2].SetFeedback(DensitySmooth);

            T bandwidthLeft  = bandwidthFilter[0](left);
            T bandwidthRight = bandwidthFilter[1](right);

            T earlyReflectionsL = static_cast<T>(
                earlyReflectionsDelayLine[0]( bandwidthLeft * 0.5 + bandwidthRight * 0.3 )
                + earlyReflectionsDelayLine[0].GetIndex(2) * 0.6
                + earlyReflectionsDelayLine[0].GetIndex(3) * 0.4
                + earlyReflectionsDelayLine[0].GetIndex(4) * 0.3
                + earlyReflectionsDelayLine[0].GetIndex(5) * 0.3
                + earlyReflectionsDelayLine[0].GetIndex(6) * 0.1
                + earlyReflectionsDelayLine[0].GetIndex(7) * 0.1
                + ( bandwidthLeft * 0.4 + bandwidthRight * 0.2 ) * 0.5);

            T earlyReflectionsR = static_cast<T>(
                earlyReflectionsDelayLine[1]( bandwidthLeft * 0.3 + bandwidthRight * 0.5 )
                + earlyReflectionsDelayLine[1].GetIndex(2) * 0.6
                + earlyReflectionsDelayLine[1].GetIndex(3) * 0.4
                + earlyReflectionsDelayLine[1].GetIndex(4) * 0.3
                + earlyReflectionsDelayLine[1].GetIndex(5) * 0.3
                + earlyReflectionsDelayLine[1].GetIndex(6) * 0.1
                + earlyReflectionsDelayLine[1].GetIndex(7) * 0.1
                + ( bandwidthLeft * 0.2 + bandwidthRight * 0.4 ) * 0.5);

            T predelayMonoInput = predelay(( bandwidthRight + bandwidthLeft ) * 0.5f);
            T smearedInput = predelayMonoInput;
            for(int j = 0; j < 4; j++)
                smearedInput = allpass[j]( smearedInput );

            T leftTank  = allpassFourTap[0]( smearedInput + PreviousRightTank );
            leftTank    = staticDelayLine[0](leftTank);
            leftTank    = damping[0](leftTank);
            leftTank    = allpassFourTap[1](leftTank);
            leftTank    = staticDelayLine[1](leftTank);

            T rightTank = allpassFourTap[2]( smearedInput + PreviousLeftTank );
            rightTank   = staticDelayLine[2](rightTank);
            rightTank   = damping[1](rightTank);
            rightTank   = allpassFourTap[3](rightTank);
            rightTank   = staticDelayLine[3](rightTank);

            PreviousLeftTank  = leftTank  * DecaySmooth;
            PreviousRightTank = rightTank * DecaySmooth;

            T accumulatorL = static_cast<T>(
                 (0.6 * staticDelayLine[2].GetIndex(1))
                +(0.6 * staticDelayLine[2].GetIndex(2))
                -(0.6 * allpassFourTap[3].GetIndex(1))
                +(0.6 * staticDelayLine[3].GetIndex(1))
                -(0.6 * staticDelayLine[0].GetIndex(1))
                -(0.6 * allpassFourTap[1].GetIndex(1))
                -(0.6 * staticDelayLine[1].GetIndex(1)));

            T accumulatorR = static_cast<T>(
                 (0.6 * staticDelayLine[0].GetIndex(2))
                +(0.6 * staticDelayLine[0].GetIndex(3))
                -(0.6 * allpassFourTap[1].GetIndex(2))
                +(0.6 * staticDelayLine[1].GetIndex(2))
                -(0.6 * staticDelayLine[2].GetIndex(3))
                -(0.6 * allpassFourTap[3].GetIndex(2))
                -(0.6 * staticDelayLine[3].GetIndex(2)));

            accumulatorL = ((accumulatorL * EarlyLateSmooth) + ((1 - EarlyLateSmooth) * earlyReflectionsL));
            accumulatorR = ((accumulatorR * EarlyLateSmooth) + ((1 - EarlyLateSmooth) * earlyReflectionsR));

            left  = ( left  + MixSmooth * ( accumulatorL - left  ) ) * Gain;
            right = ( right + MixSmooth * ( accumulatorR - right ) ) * Gain;

            outputs[0][i] = left;
            outputs[1][i] = right;
        }
    }

    void reset(){
        ControlRateCounter = 0;
        bandwidthFilter[0].SetSampleRate(SampleRate);
        bandwidthFilter[1].SetSampleRate(SampleRate);
        bandwidthFilter[0].Reset();
        bandwidthFilter[1].Reset();
        bandwidthFilter[0].Type(StateVariable<T,4>::LOWPASS);
        bandwidthFilter[1].Type(StateVariable<T,4>::LOWPASS);
        damping[0].SetSampleRate(SampleRate);
        damping[1].SetSampleRate(SampleRate);
        damping[0].Reset();
        damping[1].Reset();

        predelay.Clear();
        predelay.SetLength(static_cast<int>(PreDelayTime));

        allpass[0].Clear(); allpass[1].Clear();
        allpass[2].Clear(); allpass[3].Clear();
        allpass[0].SetLength(static_cast<int>(0.0048 * SampleRate));
        allpass[1].SetLength(static_cast<int>(0.0036 * SampleRate));
        allpass[2].SetLength(static_cast<int>(0.0127 * SampleRate));
        allpass[3].SetLength(static_cast<int>(0.0093 * SampleRate));
        allpass[0].SetFeedback(0.75);
        allpass[1].SetFeedback(0.75);
        allpass[2].SetFeedback(0.625);
        allpass[3].SetFeedback(0.625);

        allpassFourTap[0].Clear(); allpassFourTap[1].Clear();
        allpassFourTap[2].Clear(); allpassFourTap[3].Clear();
        allpassFourTap[0].SetLength(static_cast<int>(0.020 * SampleRate * Size));
        allpassFourTap[1].SetLength(static_cast<int>(0.060 * SampleRate * Size));
        allpassFourTap[2].SetLength(static_cast<int>(0.030 * SampleRate * Size));
        allpassFourTap[3].SetLength(static_cast<int>(0.089 * SampleRate * Size));
        allpassFourTap[0].SetFeedback(Density1);
        allpassFourTap[1].SetFeedback(Density2);
        allpassFourTap[2].SetFeedback(Density1);
        allpassFourTap[3].SetFeedback(Density2);
        allpassFourTap[0].SetIndex(0, 0, 0, 0);
        allpassFourTap[1].SetIndex(0, static_cast<int>(0.006 * SampleRate * Size),
                                      static_cast<int>(0.041 * SampleRate * Size), 0);
        allpassFourTap[2].SetIndex(0, 0, 0, 0);
        allpassFourTap[3].SetIndex(0, static_cast<int>(0.031 * SampleRate * Size),
                                      static_cast<int>(0.011 * SampleRate * Size), 0);

        staticDelayLine[0].Clear(); staticDelayLine[1].Clear();
        staticDelayLine[2].Clear(); staticDelayLine[3].Clear();
        staticDelayLine[0].SetLength(static_cast<int>(0.15 * SampleRate * Size));
        staticDelayLine[1].SetLength(static_cast<int>(0.12 * SampleRate * Size));
        staticDelayLine[2].SetLength(static_cast<int>(0.14 * SampleRate * Size));
        staticDelayLine[3].SetLength(static_cast<int>(0.11 * SampleRate * Size));
        staticDelayLine[0].SetIndex(0, static_cast<int>(0.067 * SampleRate * Size),
                                       static_cast<int>(0.011 * SampleRate * Size),
                                       static_cast<int>(0.121 * SampleRate * Size));
        staticDelayLine[1].SetIndex(0, static_cast<int>(0.036 * SampleRate * Size),
                                       static_cast<int>(0.089 * SampleRate * Size), 0);
        staticDelayLine[2].SetIndex(0, static_cast<int>(0.0089 * SampleRate * Size),
                                       static_cast<int>(0.099 * SampleRate * Size), 0);
        staticDelayLine[3].SetIndex(0, static_cast<int>(0.067 * SampleRate * Size),
                                       static_cast<int>(0.0041 * SampleRate * Size), 0);

        earlyReflectionsDelayLine[0].Clear();
        earlyReflectionsDelayLine[1].Clear();
        earlyReflectionsDelayLine[0].SetLength(static_cast<int>(0.089 * SampleRate));
        earlyReflectionsDelayLine[0].SetIndex(0,
            static_cast<int>(0.0199 * SampleRate),
            static_cast<int>(0.0219 * SampleRate),
            static_cast<int>(0.0354 * SampleRate),
            static_cast<int>(0.0389 * SampleRate),
            static_cast<int>(0.0414 * SampleRate),
            static_cast<int>(0.0692 * SampleRate), 0);
        earlyReflectionsDelayLine[1].SetLength(static_cast<int>(0.069 * SampleRate));
        earlyReflectionsDelayLine[1].SetIndex(0,
            static_cast<int>(0.0099 * SampleRate),
            static_cast<int>(0.011  * SampleRate),
            static_cast<int>(0.0182 * SampleRate),
            static_cast<int>(0.0189 * SampleRate),
            static_cast<int>(0.0213 * SampleRate),
            static_cast<int>(0.0431 * SampleRate), 0);
    }

    void setParameter(int index, T value){
        switch(index){
            case DAMPINGFREQ:
                DampingFreq = static_cast<T>(1. - value);
                break;
            case DENSITY:
                Density1 = value;
                break;
            case BANDWIDTHFREQ:
                BandwidthFreq = value;
                break;
            case PREDELAY:
                PreDelayTime = value;
                break;
            case SIZE:
                Size = static_cast<T>((0.95 * value) + 0.05);
                allpassFourTap[0].Clear(); allpassFourTap[1].Clear();
                allpassFourTap[2].Clear(); allpassFourTap[3].Clear();
                allpassFourTap[0].SetLength(static_cast<int>(0.020 * SampleRate * Size));
                allpassFourTap[1].SetLength(static_cast<int>(0.060 * SampleRate * Size));
                allpassFourTap[2].SetLength(static_cast<int>(0.030 * SampleRate * Size));
                allpassFourTap[3].SetLength(static_cast<int>(0.089 * SampleRate * Size));
                allpassFourTap[1].SetIndex(0, static_cast<int>(0.006 * SampleRate * Size),
                                              static_cast<int>(0.041 * SampleRate * Size), 0);
                allpassFourTap[3].SetIndex(0, static_cast<int>(0.031 * SampleRate * Size),
                                              static_cast<int>(0.011 * SampleRate * Size), 0);
                staticDelayLine[0].Clear(); staticDelayLine[1].Clear();
                staticDelayLine[2].Clear(); staticDelayLine[3].Clear();
                staticDelayLine[0].SetLength(static_cast<int>(0.15 * SampleRate * Size));
                staticDelayLine[1].SetLength(static_cast<int>(0.12 * SampleRate * Size));
                staticDelayLine[2].SetLength(static_cast<int>(0.14 * SampleRate * Size));
                staticDelayLine[3].SetLength(static_cast<int>(0.11 * SampleRate * Size));
                staticDelayLine[0].SetIndex(0, static_cast<int>(0.067 * SampleRate * Size),
                                               static_cast<int>(0.011 * SampleRate * Size),
                                               static_cast<int>(0.121 * SampleRate * Size));
                staticDelayLine[1].SetIndex(0, static_cast<int>(0.036 * SampleRate * Size),
                                               static_cast<int>(0.089 * SampleRate * Size), 0);
                staticDelayLine[2].SetIndex(0, static_cast<int>(0.0089 * SampleRate * Size),
                                               static_cast<int>(0.099 * SampleRate * Size), 0);
                staticDelayLine[3].SetIndex(0, static_cast<int>(0.067 * SampleRate * Size),
                                               static_cast<int>(0.0041 * SampleRate * Size), 0);
                break;
            case DECAY:
                Decay = value;
                break;
            case GAIN:
                Gain = value;
                break;
            case MIX:
                Mix = value;
                break;
            case EARLYMIX:
                EarlyMix = value;
                break;
        }
    }

    float getParameter(int index){
        switch(index){
            case DAMPINGFREQ:   return DampingFreq * 100.f;
            case DENSITY:       return Density1    * 100.f;
            case BANDWIDTHFREQ: return BandwidthFreq * 100.f;
            case PREDELAY:      return PreDelayTime  * 100.f;
            case SIZE:          return static_cast<float>(((0.95 * Size) + 0.05) * 100.);
            case DECAY:         return Decay  * 100.f;
            case GAIN:          return Gain   * 100.f;
            case MIX:           return Mix    * 100.f;
            case EARLYMIX:      return EarlyMix * 100.f;
            default:            return 0.f;
        }
    }

    void setSampleRate(T sr){
        SampleRate = sr;
        ControlRate = static_cast<int>(SampleRate / 1000);
        reset();
    }
};

// ============================================================================
// Allpass
// ============================================================================
template<typename T, int maxLength>
class Allpass
{
private:
    T   buffer[maxLength] = {};
    int index  = 0;
    int Length = 0;
    T   Feedback = {};

public:
    Allpass(){
        SetLength(maxLength - 1);
        Clear();
        Feedback = 0.5;
    }

    T operator()(T input){
        T bufout = buffer[index];
        T temp   = input * -Feedback;
        T output = bufout + temp;
        buffer[index] = input + ((bufout + temp) * Feedback);
        if(++index >= Length) index = 0;
        return output;
    }

    void SetLength(int len){
        if(len >= maxLength) len = maxLength;
        if(len < 0)          len = 0;
        Length = len;
    }

    void SetFeedback(T feedback){ Feedback = feedback; }

    void Clear(){
        memset(buffer, 0, sizeof(buffer));
        index = 0;
    }

    int GetLength() const { return Length; }
};

// ============================================================================
// StaticAllpassFourTap
// ============================================================================
template<typename T, int maxLength>
class StaticAllpassFourTap
{
private:
    T   buffer[maxLength] = {};
    int index1 = 0, index2 = 0, index3 = 0, index4 = 0;
    int Length = 0;
    T   Feedback = {};

public:
    StaticAllpassFourTap(){
        SetLength(maxLength - 1);
        Clear();
        Feedback = 0.5;
    }

    T operator()(T input){
        T bufout = buffer[index1];
        T temp   = input * -Feedback;
        T output = bufout + temp;
        buffer[index1] = input + ((bufout + temp) * Feedback);
        if(++index1 >= Length) index1 = 0;
        if(++index2 >= Length) index2 = 0;
        if(++index3 >= Length) index3 = 0;
        if(++index4 >= Length) index4 = 0;
        return output;
    }

    void SetIndex(int i1, int i2, int i3, int i4){
        index1 = i1; index2 = i2; index3 = i3; index4 = i4;
    }

    T GetIndex(int idx){
        switch(idx){
            case 0: return buffer[index1];
            case 1: return buffer[index2];
            case 2: return buffer[index3];
            case 3: return buffer[index4];
            default: return buffer[index1];
        }
    }

    void SetLength(int len){
        if(len >= maxLength) len = maxLength;
        if(len < 0)          len = 0;
        Length = len;
    }

    void Clear(){
        memset(buffer, 0, sizeof(buffer));
        index1 = index2 = index3 = index4 = 0;
    }

    void SetFeedback(T feedback){ Feedback = feedback; }

    int GetLength() const { return Length; }
};

// ============================================================================
// StaticDelayLine
// ============================================================================
template<typename T, int maxLength>
class StaticDelayLine
{
private:
    T   buffer[maxLength] = {};
    int index  = 0;
    int Length = 0;
    T   Feedback = {};

public:
    StaticDelayLine(){
        SetLength(maxLength - 1);
        Clear();
    }

    T operator()(T input){
        T output = buffer[index];
        buffer[index++] = input;
        if(index >= Length) index = 0;
        return output;
    }

    void SetLength(int len){
        if(len >= maxLength) len = maxLength;
        if(len < 0)          len = 0;
        Length = len;
    }

    void Clear(){
        memset(buffer, 0, sizeof(buffer));
        index = 0;
    }

    int GetLength() const { return Length; }
};

// ============================================================================
// StaticDelayLineFourTap
// ============================================================================
template<typename T, int maxLength>
class StaticDelayLineFourTap
{
private:
    T   buffer[maxLength] = {};
    int index1 = 0, index2 = 0, index3 = 0, index4 = 0;
    int Length = 0;
    T   Feedback = {};

public:
    StaticDelayLineFourTap(){
        SetLength(maxLength - 1);
        Clear();
    }

    T operator()(T input){
        T output = buffer[index1];
        buffer[index1++] = input;
        if(index1 >= Length) index1 = 0;
        if(++index2 >= Length) index2 = 0;
        if(++index3 >= Length) index3 = 0;
        if(++index4 >= Length) index4 = 0;
        return output;
    }

    void SetIndex(int i1, int i2, int i3, int i4){
        index1 = i1; index2 = i2; index3 = i3; index4 = i4;
    }

    T GetIndex(int idx){
        switch(idx){
            case 0: return buffer[index1];
            case 1: return buffer[index2];
            case 2: return buffer[index3];
            case 3: return buffer[index4];
            default: return buffer[index1];
        }
    }

    void SetLength(int len){
        if(len >= maxLength) len = maxLength;
        if(len < 0)          len = 0;
        Length = len;
    }

    void Clear(){
        memset(buffer, 0, sizeof(buffer));
        index1 = index2 = index3 = index4 = 0;
    }

    int GetLength() const { return Length; }
};

// ============================================================================
// StaticDelayLineEightTap
// ============================================================================
template<typename T, int maxLength>
class StaticDelayLineEightTap
{
private:
    T   buffer[maxLength] = {};
    int index1=0, index2=0, index3=0, index4=0,
        index5=0, index6=0, index7=0, index8=0;
    int Length = 0;
    T   Feedback = {};

public:
    StaticDelayLineEightTap(){
        SetLength(maxLength - 1);
        Clear();
    }

    T operator()(T input){
        T output = buffer[index1];
        buffer[index1++] = input;
        if(index1 >= Length) index1 = 0;
        if(++index2 >= Length) index2 = 0;
        if(++index3 >= Length) index3 = 0;
        if(++index4 >= Length) index4 = 0;
        if(++index5 >= Length) index5 = 0;
        if(++index6 >= Length) index6 = 0;
        if(++index7 >= Length) index7 = 0;
        if(++index8 >= Length) index8 = 0;
        return output;
    }

    void SetIndex(int i1, int i2, int i3, int i4,
                  int i5, int i6, int i7, int i8){
        index1=i1; index2=i2; index3=i3; index4=i4;
        index5=i5; index6=i6; index7=i7; index8=i8;
    }

    T GetIndex(int idx){
        switch(idx){
            case 0: return buffer[index1];
            case 1: return buffer[index2];
            case 2: return buffer[index3];
            case 3: return buffer[index4];
            case 4: return buffer[index5];
            case 5: return buffer[index6];
            case 6: return buffer[index7];
            case 7: return buffer[index8];
            default: return buffer[index1];
        }
    }

    void SetLength(int len){
        if(len >= maxLength) len = maxLength;
        if(len < 0)          len = 0;
        Length = len;
    }

    void Clear(){
        memset(buffer, 0, sizeof(buffer));
        index1=index2=index3=index4=index5=index6=index7=index8=0;
    }

    int GetLength() const { return Length; }
};

// ============================================================================
// StateVariable filter (4x oversampled)
// ============================================================================
template<typename T, int OverSampleCount>
class StateVariable
{
public:
    enum FilterType { LOWPASS, HIGHPASS, BANDPASS, NOTCH, FilterTypeCount };

private:
    T sampleRate = {};
    T frequency  = {};
    T q = {};
    T f = {};
    T low = {}, high = {}, band = {}, notch = {};
    T *out = nullptr;

public:
    StateVariable(){
        SetSampleRate(44100.);
        Frequency(1000.);
        Resonance(0);
        Type(LOWPASS);
        Reset();
    }

    T operator()(T input){
        for(int i = 0; i < OverSampleCount; ++i){
            low  += static_cast<T>(f * band + 1e-25);
            high  = input - low - q * band;
            band += f * high;
            notch = low + high;
        }
        return *out;
    }

    void Reset(){ low = high = band = notch = 0; }

    void SetSampleRate(T sr){
        sampleRate = sr * OverSampleCount;
        UpdateCoefficient();
    }

    void Frequency(T freq){
        frequency = freq;
        UpdateCoefficient();
    }

    void Resonance(T resonance){ q = 2 - 2 * resonance; }

    void Type(int type){
        switch(type){
            case LOWPASS:  out = &low;   break;
            case HIGHPASS: out = &high;  break;
            case BANDPASS: out = &band;  break;
            case NOTCH:    out = &notch; break;
            default:       out = &low;   break;
        }
    }

private:
    void UpdateCoefficient(){
        f = static_cast<T>(2. * sinf(3.141592654f * frequency / sampleRate));
    }
};

// ============================================================================
// Reverb::Impl — wraps MVerb<float>
// ============================================================================
struct Reverb::Impl {
    MVerb<float> mverb;

    explicit Impl(double sr, float /*decay_time*/, float /*wet_mix*/) {
        mverb.setSampleRate(static_cast<float>(sr));
        mverb.setParameter(MVerb<float>::DAMPINGFREQ, 0.9f);
        mverb.setParameter(MVerb<float>::DENSITY,     0.75f);
        mverb.setParameter(MVerb<float>::BANDWIDTHFREQ, 1.0f);
        mverb.setParameter(MVerb<float>::DECAY,       0.5f);
        mverb.setParameter(MVerb<float>::PREDELAY,    0.f);
        mverb.setParameter(MVerb<float>::SIZE,        0.5f);
        mverb.setParameter(MVerb<float>::GAIN,        1.0f);
        mverb.setParameter(MVerb<float>::MIX,         0.5f);
        mverb.setParameter(MVerb<float>::EARLYMIX,    0.9f);
    }

    // Map seconds [0, 20] → MVerb DECAY [0, 1]
    void set_decay_time(float seconds) {
        const float d = std::clamp(seconds / 20.0f, 0.0f, 1.0f);
        mverb.setParameter(MVerb<float>::DECAY, d);
    }

    void set_wet_mix(float mix) {
        mverb.setParameter(MVerb<float>::MIX, std::clamp(mix, 0.0f, 1.0f));
    }

    // damping: 0 = bright, 1 = dark (mapped to MVerb DAMPINGFREQ which inverts internally)
    void set_damping(float damping) {
        mverb.setParameter(MVerb<float>::DAMPINGFREQ, std::clamp(damping, 0.0f, 0.95f));
    }

    void set_bandwidth(float bandwidth) {
        mverb.setParameter(MVerb<float>::BANDWIDTHFREQ, std::clamp(bandwidth, 0.0f, 1.0f));
    }
};

// ============================================================================
// Reverb public API
// ============================================================================
Reverb::Reverb(double sample_rate, float decay_time, float wet_mix)
    : m_impl(std::make_unique<Impl>(sample_rate, decay_time, wet_mix))
{}

Reverb::~Reverb() = default;

void Reverb::set_decay_time(float seconds) {
    m_impl->set_decay_time(std::max(0.01f, seconds));
}

void Reverb::set_wet_mix(float mix) {
    m_impl->set_wet_mix(mix);
}

void Reverb::set_damping(float damping) {
    m_impl->set_damping(damping);
}

void Reverb::set_bandwidth(float bandwidth) {
    m_impl->set_bandwidth(bandwidth);
}

void Reverb::process_samples_inplace(float* buffer, size_t frames, unsigned channels) {
    if (!buffer || frames == 0 || channels == 0) return;

    // MVerb expects two separate channel buffers (non-interleaved stereo).
    // De-interleave input into L/R, process, re-interleave.
    std::vector<float> left(frames), right(frames);

    if (channels >= 2) {
        for (size_t f = 0; f < frames; ++f) {
            left[f]  = buffer[f * channels + 0];
            right[f] = buffer[f * channels + 1];
        }
    } else {
        // Mono: duplicate to both channels
        for (size_t f = 0; f < frames; ++f)
            left[f] = right[f] = buffer[f];
    }

    float* ins[2]  = { left.data(),  right.data() };
    float* outs[2] = { left.data(),  right.data() };
    m_impl->mverb.process(ins, outs, static_cast<int>(frames));

    if (channels >= 2) {
        for (size_t f = 0; f < frames; ++f) {
            buffer[f * channels + 0] = left[f];
            buffer[f * channels + 1] = right[f];
            // Pass through any extra channels unchanged
            for (unsigned ch = 2; ch < channels; ++ch)
                buffer[f * channels + ch] = buffer[f * channels + ch];
        }
    } else {
        for (size_t f = 0; f < frames; ++f)
            buffer[f] = (left[f] + right[f]) * 0.5f;
    }
}
