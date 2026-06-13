/*
 * gen_freedv.c - FreeDV test-signal generator.
 *
 * Takes a real 8 kHz / 16-bit mono speech recording, modulates it with the
 * FreeDV transmitter for a chosen mode, embeds a repeating text-channel string,
 * adds a controllable amount of white Gaussian noise (target channel SNR in a
 * 3000 Hz noise bandwidth), and writes the result as an 8 kHz / 16-bit mono
 * WAV file. The output is a valid off-air-like FreeDV recording that can be
 * fed to the decoder test harness, to this SDR++ module (Tools -> File source),
 * or to freedv-gui itself.
 *
 * Build: see the test/ section of the module README.
 */
#include <codec2/freedv_api.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* --- minimal WAV writer (PCM16 mono) --------------------------------------*/
static void write_wav(const char* path, const short* s, int n, int fs) {
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    int byteRate = fs * 2;
    int dataBytes = n * 2;
    int riff = 36 + dataBytes;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    int sub1 = 16; short fmt = 1, ch = 1, bits = 16, ba = 2;
    fwrite(&sub1, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&fs, 4, 1, f); fwrite(&byteRate, 4, 1, f); fwrite(&ba, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
    fwrite(s, 2, n, f);
    fclose(f);
}

/* --- Gaussian noise (Box-Muller) ------------------------------------------*/
static double randn(void) {
    double u1 = (rand() + 1.0) / (RAND_MAX + 2.0);
    double u2 = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

static int mode_from_name(const char* n) {
    if (!strcmp(n, "1600"))  return FREEDV_MODE_1600;
    if (!strcmp(n, "700C"))  return FREEDV_MODE_700C;
    if (!strcmp(n, "700D"))  return FREEDV_MODE_700D;
    if (!strcmp(n, "700E"))  return FREEDV_MODE_700E;
    if (!strcmp(n, "800XA")) return FREEDV_MODE_800XA;
    if (!strcmp(n, "2020"))  return FREEDV_MODE_2020;
    if (!strcmp(n, "2020B")) return FREEDV_MODE_2020B;
    return -1;
}

/* Tx text-channel callback: feed the next char of a repeating string. */
static const char* g_txt = "FREEDV TEST ";
static int g_txtPos = 0;
static char txt_tx_cb(void* state) {
    (void)state;
    char c = g_txt[g_txtPos];
    g_txtPos = (g_txtPos + 1) % (int)strlen(g_txt);
    return c;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr,
            "usage: %s <mode> <snr_db> <in.raw(8k/16/mono)> <out.wav> [text] [seed]\n"
            "  mode: 1600 700C 700D 700E 800XA 2020 2020B\n", argv[0]);
        return 1;
    }
    int mode = mode_from_name(argv[1]);
    if (mode < 0) { fprintf(stderr, "unknown mode %s\n", argv[1]); return 1; }
    double snr_db = atof(argv[2]);
    const char* inPath = argv[3];
    const char* outPath = argv[4];
    if (argc >= 6) { g_txt = argv[5]; }
    unsigned seed = (argc >= 7) ? (unsigned)atoi(argv[6]) : 12345u;
    srand(seed);

    struct freedv* fdv = freedv_open(mode);
    if (!fdv) { fprintf(stderr, "freedv_open failed for %s\n", argv[1]); return 1; }
    freedv_set_callback_txt(fdv, NULL, txt_tx_cb, NULL);

    int nSpeech = freedv_get_n_speech_samples(fdv);
    int nModem = freedv_get_n_nom_modem_samples(fdv);
    int speechFs = freedv_get_speech_sample_rate(fdv);
    int modemFs = freedv_get_modem_sample_rate(fdv);

    /* Read the whole input speech file. */
    FILE* fin = fopen(inPath, "rb");
    if (!fin) { fprintf(stderr, "cannot open %s\n", inPath); return 1; }
    fseek(fin, 0, SEEK_END);
    long bytes = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    int inSamples = (int)(bytes / 2);
    short* speechIn = (short*)malloc(sizeof(short) * (inSamples + nSpeech));
    memset(speechIn, 0, sizeof(short) * (inSamples + nSpeech));
    size_t got = fread(speechIn, 2, inSamples, fin);
    fclose(fin);
    (void)got;

    /* The input file is 8 kHz. If the mode's speech rate is 16 kHz (2020),
       naively duplicate samples so the duration is preserved. This is only a
       test stimulus, not a fidelity benchmark. */
    if (speechFs == 16000) {
        short* up = (short*)malloc(sizeof(short) * inSamples * 2);
        for (int i = 0; i < inSamples; i++) { up[2*i] = up[2*i+1] = speechIn[i]; }
        free(speechIn);
        speechIn = up;
        inSamples *= 2;
    }

    /* Modulate frame by frame. */
    int nFrames = inSamples / nSpeech;
    int totalModem = nFrames * nModem;
    short* mod = (short*)malloc(sizeof(short) * totalModem);
    short* frame = (short*)malloc(sizeof(short) * nModem);
    for (int fr = 0; fr < nFrames; fr++) {
        freedv_tx(fdv, frame, &speechIn[fr * nSpeech]);
        memcpy(&mod[fr * nModem], frame, sizeof(short) * nModem);
    }

    /* Measure signal power and add AWGN for the requested channel SNR.
       FreeDV defines SNR in a 3000 Hz noise bandwidth; the noise we add is
       white across the full modemFs/2 band, so we scale the noise variance by
       (modemFs/2)/3000 so the reported "channel SNR" refers to that 3000 Hz
       reference, matching freedv-gui's convention. */
    double sigPow = 0.0;
    for (int i = 0; i < totalModem; i++) {
        double v = (double)mod[i];
        sigPow += v * v;
    }
    sigPow /= (totalModem > 0 ? totalModem : 1);

    double bwScale = (double)(modemFs / 2) / 3000.0;
    double noisePow = (sigPow / pow(10.0, snr_db / 10.0)) * bwScale;
    double noiseStd = sqrt(noisePow);

    short* out = (short*)malloc(sizeof(short) * totalModem);
    for (int i = 0; i < totalModem; i++) {
        double v = (double)mod[i] + noiseStd * randn();
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        out[i] = (short)lround(v);
    }

    write_wav(outPath, out, totalModem, modemFs);

    printf("mode=%s snr=%.1f dB  frames=%d  modem_fs=%d  speech_fs=%d  "
           "out=%s (%d samples, %.1f s)  text=\"%s\"\n",
           argv[1], snr_db, nFrames, modemFs, speechFs, outPath,
           totalModem, (double)totalModem / modemFs, g_txt);

    free(speechIn); free(mod); free(frame); free(out);
    freedv_close(fdv);
    return 0;
}
