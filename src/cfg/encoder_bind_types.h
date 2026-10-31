#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CTRL_VOL = 0,
    CTRL_SQL,
    CTRL_RFG,
    CTRL_FILTER_LOW,
    CTRL_FILTER_HIGH,
    CTRL_PWR,
    CTRL_HMIC,
    CTRL_MIC,
    CTRL_IMIC,
    CTRL_MONI,
    CTRL_FILTER_BW,

    CTRL_SPECTRUM_FACTOR,

    CTRL_KEY_SPEED,
    CTRL_KEY_MODE,
    CTRL_IAMBIC_MODE,
    CTRL_KEY_TONE,
    CTRL_KEY_VOL,
    CTRL_KEY_TRAIN,
    CTRL_QSK_TIME,
    CTRL_KEY_RATIO,

    CTRL_DNF,
    CTRL_DNF_CENTER,
    CTRL_DNF_WIDTH,
    CTRL_DNF_AUTO,
    CTRL_NB,
    CTRL_NB_LEVEL,
    CTRL_NB_WIDTH,
    CTRL_NR,
    CTRL_NR_LEVEL,

    CTRL_AGC_HANG,
    CTRL_AGC_KNEE,
    CTRL_AGC_SLOPE,
    CTRL_COMP,

    CTRL_CW_DECODER,
    CTRL_CW_TUNE,
    CTRL_CW_DECODER_SNR,
    CTRL_CW_DECODER_PEAK_BETA,
    CTRL_CW_DECODER_NOISE_BETA,

    CTRL_ANT,
    CTRL_RIT,
    CTRL_XIT,
    CTRL_IF_SHIFT,

    CTRL_VOX_ON,
    CTRL_VOX_GAIN,
    CTRL_VOX_AG,
    CTRL_VOX_DELAY,

    CTRL_CW_PEAK_ON,
    CTRL_CW_PEAK_Q,
    CTRL_CW_ZAP,

    /* New controls go here, after the others (the encoder binds are stored
     * by position) and before CTRL_FAST_ACCESS_LAST (bindable) */
    CTRL_WF_REF,
    CTRL_CLUSTER,     /* MFK only, offered while the cluster column is shown; not stored */
    CTRL_WF_MODE,
    CTRL_CLUSTER_ORDER,   /* as CTRL_CLUSTER: MFK only, while the column is shown */
    CTRL_CLUSTER_BANDS,
    CTRL_CLUSTER_MODES,   /* as CTRL_CLUSTER: All, CW, SSB, DIG, CW+DIG, the filter of Settings */
    CTRL_AGC,             /* Off, Slow, Fast, Auto, as the AGC key */
    CTRL_PRE,
    CTRL_ATT,
    CTRL_ATU,             /* the tuner on or off, as a press of the ATU key */
    CTRL_STEP,            /* the tuning step, 10 Hz to 5 kHz, as the FST key */

    CTRL_FAST_ACCESS_LAST,

    /* APPs */

    CTRL_RTTY_RATE,
    CTRL_RTTY_SHIFT,
    CTRL_RTTY_CENTER,
    CTRL_RTTY_REVERSE,

    CTRL_WEFAX_ALIGN,     /* WEFAX app: picture shift */
    CTRL_WEFAX_TILT,      /* and slant */
    CTRL_LAST,
} cfg_ctrl_t;

typedef enum {
    ENCODER_BIND_NONE = '_',
    ENCODER_BIND_VOL  = 'V',
    ENCODER_BIND_MFK  = 'M'
} encoder_binds_t;

#ifdef __cplusplus
}
#endif
