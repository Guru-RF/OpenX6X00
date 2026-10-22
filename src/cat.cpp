/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

/*
 * X6100 protocol implementation (Mfg 3087)
 */

#include "cat.h"
#include "board.h"

#include "cfg/settings_manager.h"
#include "common/queue.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <pthread.h>
#include <vector>
#include <cmath>
#include <chrono>

extern "C" {
    #include "events.h"
    #include "meter.h"
    #include "radio.h"
    #include "scheduler.h"
    #include "spectrum.h"
    #include "waterfall.h"
    #include "tx_info.h"

    #include "lvgl/lvgl.h"
    #include <fcntl.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #include <sys/poll.h>
    #include <termios.h>
    #include <unistd.h>
}

// Resolve the frequency parameter for a CI-V main/sub VFO selector. vfo_id is
// the CI-V selector (RADIO_VFO_A = main/selected VFO, RADIO_VFO_B = sub/inactive
// VFO); cur_vfo identifies which hardware VFO (A or B) is currently active.
static Parameter<int32_t>& vfo_freq(int vfo_id, int cur_vfo)
{
    if (vfo_id == RADIO_VFO_A)
    {
        return cur_vfo == RADIO_VFO_A ? cfg_sm.p_band_vfoa_freq : cfg_sm.p_band_vfob_freq;
    }
    else
    {
        return cur_vfo == RADIO_VFO_A ? cfg_sm.p_band_vfob_freq : cfg_sm.p_band_vfoa_freq;
    }
}

static Parameter<int32_t>& vfo_mode(int vfo_id, int cur_vfo)
{
    if (vfo_id == RADIO_VFO_A)
    {
        return cur_vfo == RADIO_VFO_A ? cfg_sm.p_band_vfoa_mode : cfg_sm.p_band_vfob_mode;
    }
    else
    {
        return cur_vfo == RADIO_VFO_A ? cfg_sm.p_band_vfob_mode : cfg_sm.p_band_vfoa_mode;
    }
}

void to_bcd(uint8_t bcd_data[], uint64_t data, uint8_t len) {
    int16_t i;

    for (i = 0; i < len / 2; i++) {
        uint8_t a = data % 10;

        data /= 10;
        a |= (data % 10) << 4;
        data /= 10;
        bcd_data[i] = a;
    }

    if (len & 1) {
        bcd_data[i] &= 0x0f;
        bcd_data[i] |= data % 10;
    }
}

void to_bcd_be(uint8_t bcd_data[], uint64_t data, uint8_t len) {
    int16_t i;

    /* Whole bytes only: the old odd-length tail ran with i == -1 and wrote
     * into the byte before bcd_data (the echoed subcommand) */
    for (i = (len - 1) / 2; i >= 0; i--) {
        uint8_t a = data % 10;

        data /= 10;
        a |= (data % 10) << 4;
        data /= 10;
        bcd_data[i] = a;
    }

}

uint64_t from_bcd(const uint8_t bcd_data[], uint8_t len) {
    int16_t     i;
    uint64_t    data = 0;

    if (len & 1) {
        data = bcd_data[len / 2] & 0x0F;
    }

    for (i = (len / 2) - 1; i >= 0; i--) {
        data *= 10;
        data += bcd_data[i] >> 4;
        data *= 10;
        data += bcd_data[i] & 0x0F;
    }

    return data;
}

uint64_t from_bcd_be(const uint8_t bcd_data[], uint8_t len) {
    int16_t     i = 0;
    uint64_t    data = 0;

    if (len & 1) {
        data = bcd_data[0] & 0x0F;
        i++;
    }

    for (; i <= (len / 2); i++) {
        data *= 10;
        data += bcd_data[i] >> 4;
        data *= 10;
        data += bcd_data[i] & 0x0F;
    }

    return data;
}

#define FRAME_PRE 0xFE
#define FRAME_END 0xFD

#define CODE_OK 0xFB
#define CODE_NG 0xFA

#define LOCAL_ADDRESS 0xA4

#define C_SND_FREQ 0x00      /* Send frequency data  transceive mode does not ack*/
#define C_SND_MODE 0x01      /* Send mode data, Sc  for transceive mode does not ack */
#define C_RD_BAND 0x02       /* Read band edge frequencies */
#define C_RD_FREQ 0x03       /* Read display frequency */
#define C_RD_MODE 0x04       /* Read display mode */
#define C_SET_FREQ 0x05      /* Set frequency data(1) */
#define C_SET_MODE 0x06      /* Set mode data, Sc */
#define C_SET_VFO 0x07       /* Set VFO */
#define C_SET_MEM 0x08       /* Set channel, Sc(2) */
#define C_WR_MEM 0x09        /* Write memory */
#define C_MEM2VFO 0x0a       /* Memory to VFO */
#define C_CLR_MEM 0x0b       /* Memory clear */
#define C_RD_OFFS 0x0c       /* Read duplex offset frequency; default changes with HF/6M/2M */
#define C_SET_OFFS 0x0d      /* Set duplex offset frequency */
#define C_CTL_SCAN 0x0e      /* Control scan, Sc */
#define C_CTL_SPLT 0x0f      /* Control split, and duplex mode Sc */
#define C_SET_TS 0x10        /* Set tuning step, Sc */
#define C_CTL_ATT 0x11       /* Set/get attenuator, Sc */
#define C_CTL_ANT 0x12       /* Set/get antenna, Sc */
#define C_CTL_ANN 0x13       /* Control announce (speech synth.), Sc */
#define C_CTL_LVL 0x14       /* Set AF/RF/squelch, Sc */
#define C_RD_SQSM 0x15       /* Read squelch condition/S-meter level, Sc */
#define C_CTL_FUNC 0x16      /* Function settings (AGC,NB,etc.), Sc */
#define C_SND_CW 0x17        /* Send CW message */
#define C_SET_PWR 0x18       /* Set Power ON/OFF, Sc */
#define C_RD_TRXID 0x19      /* Read transceiver ID code */
#define C_CTL_MEM 0x1a       /* Misc memory/bank/rig control functions, Sc */
#define C_SET_TONE 0x1b      /* Set tone frequency */
#define C_CTL_PTT 0x1c       /* Control Transmit On/Off, Sc */
#define C_CTL_EDGE 0x1e      /* Band edges */
#define C_CTL_DVT 0x1f       /* Digital modes calsigns & messages */
#define C_CTL_DIG 0x20       /* Digital modes settings & status */
#define C_CTL_RIT 0x21       /* RIT/XIT control */
#define C_CTL_DSD 0x22       /* D-STAR Data */
#define C_SEND_SEL_FREQ 0x25 /* Send/Recv sel/unsel VFO frequency */
#define C_SEND_SEL_MODE 0x26
#define C_CTL_SCP 0x27   /* Scope control & data */
#define C_SND_VOICE 0x28 /* Transmit Voice Memory Contents */
#define C_CTL_MTEXT 0x70 /* Microtelecom Extension */
#define C_CTL_MISC 0x7f  /* Miscellaneous control, Sc */

#define S_VFOA 0x00      /* Set to VFO A */
#define S_VFOB 0x01      /* Set to VFO B */
#define S_BTOA 0xa0      /* VFO A=B */
#define S_XCHNG 0xb0     /* Switch VFO A and B */
#define S_SUBTOMAIN 0xb1 /* MAIN = SUB */
#define S_DUAL_OFF 0xc0  /* Dual watch off */
#define S_DUAL_ON 0xc1   /* Dual watch on */
#define S_DUAL 0xc2      /* Dual watch (0 = off, 1 = on) */
#define S_MAIN 0xd0      /* Select MAIN band */
#define S_SUB 0xd1       /* Select SUB band */
#define S_SUB_SEL 0xd2   /* Read/Set Main/Sub selection */
#define S_FRONTWIN 0xe0  /* Select front window */

// modes
#define M_LSB 0x00
#define M_USB 0x01
#define M_AM 0x02
#define M_CW 0x03
#define M_NFM 0x05
#define M_CWR 0x07

// memory/bank/rig control
#define MEM_BS_REG 0x01 /* Get band stacking register */
#define MEM_IF_FW 0x03  /* Get IF filter width */
#define MEM_LOCK 0x05   /* LOCK status */
#define MEM_DM_FG 0x06  /* Get data mode switch and filter group */

#define FRAME_ADD_LEN 5 /* Header and end len */

static TSQueue<std::vector<char>> send_queue;


static void on_fg_freq_change(Subject *s, void *user_data);
static void on_mode_change(Subject *s, void *user_data);
static void on_vfo_change(Subject *s, void *user_data);

struct Frame {
    uint8_t dst_addr;
    uint8_t src_addr;
    uint8_t command;
    std::vector<uint8_t> data;

    Frame(uint8_t dst, uint8_t src, uint8_t command): dst_addr(dst), src_addr(src), command(command) {};

    Frame(const char * data, const uint16_t len): data(len - FRAME_ADD_LEN - 1) {
        dst_addr = data[2];
        src_addr = data[3];
        command = data[4];
        std::copy(&data[5], &data[len - 1], this->data.begin());
    };

    Frame(const Frame *req) {
        // Swap address
        dst_addr = req->src_addr;
        src_addr = LOCAL_ADDRESS;
        command  = req->command;
        data = req->data;
    }

    void log(const char *prefix=nullptr) const {
        char buf[512];
        char *buf_ptr = buf;
        buf_ptr += sprintf(buf_ptr, "[%02X:%02X:", FRAME_PRE, FRAME_PRE);
        buf_ptr += sprintf(buf_ptr, "%02X:", dst_addr);
        buf_ptr += sprintf(buf_ptr, "%02X]-", src_addr);
        buf_ptr += sprintf(buf_ptr, "[%02X:", command);
        uint16_t remain_len = data.size();
        size_t i = 0;
        while (remain_len) {
            buf_ptr += sprintf(buf_ptr, "%02X:", data[i++]);
            remain_len--;
        }
        buf_ptr += sprintf(buf_ptr - 1, "]-[%02X]", FRAME_END);
        *(buf_ptr - 1) = '\0';
        if (prefix) {
            LV_LOG_USER("%s\t: %s\t(Len %i)", prefix, buf, data.size() + FRAME_ADD_LEN + 1);
        } else {
            LV_LOG_USER("%s\t(Len %i)", buf, data.size() + FRAME_ADD_LEN + 1);
        }
    }

    void set_code(uint8_t code) {
        set_payload_len(1);
        command = code;
    }

    void set_payload_len(size_t len) {
        data.resize(len - 1);
    }

    size_t get_len() const {
        return data.size() + 1 + FRAME_ADD_LEN;
    }

    std::vector<char> dump() const {
        std::vector<char> buf(data.size() + 1 + FRAME_ADD_LEN);
        buf[0] = FRAME_PRE;
        buf[1] = FRAME_PRE;
        buf[2] = dst_addr;
        buf[3] = src_addr;
        buf[4] = command;
        std::copy(data.begin(), data.end(), buf.begin() + 5);
        *--buf.end() = FRAME_END;
        return buf;
    }
};


class Connection {
    int        fd;
    char       buf[1024];
    const char header[2] = {FRAME_PRE, FRAME_PRE};
    size_t     start=0;
    size_t     end=0;

  protected:
    void write_buf(const char *buf, size_t len) {
        ssize_t l;
        while (len) {
            l = write(fd, buf, len);
            if (l < 0) {
                perror("Error during writing message");
                return;
            }
            len -= l;
        }
    }

  public:
    Connection(int fd) : fd(fd) {}
    Frame *feed() {
        if (start) {
            end -= start;
            memmove(buf, buf + start, end);
            start = 0;
        }
        while (1) {
            /* What is in the buffer first: frames that arrived together (a burst, a program that
             * does not wait for the answer) were answered one per later byte otherwise */
            if (end >= sizeof(header)) {
                char *frame_start = (char *)memmem(buf, end, header, sizeof(header));
                if (frame_start == NULL) {
                    // Move last byte to begin: it may be the first half of a header
                    buf[0] = buf[end - 1];
                    end    = 1;
                } else {
                    if (frame_start != buf) {
                        end -= frame_start - buf;
                        memmove(buf, frame_start, end);
                    }
                    if (end >= FRAME_ADD_LEN) {
                        char *end_pos = (char *)memchr(buf + FRAME_ADD_LEN, FRAME_END, end - FRAME_ADD_LEN);
                        if (end_pos) {
                            size_t frame_len = end_pos - buf + 1;
                            start            = frame_len;   /* what follows stays for the next call */
                            return new Frame(buf, frame_len);
                        }
                    }
                    if (end == sizeof(buf)) {
                        end = 0;    /* a header and no end in a full buffer: not a frame, dropped */
                    }
                }
            }
            int res = read(fd, buf + end, sizeof(buf) - end);
            if (res <= 0) {
                return nullptr;     /* nothing more now (or the port is gone) */
            }
            end += res;
        }
    }

    void send(const char * data, size_t len) {
        write_buf(data, len);
    }
    void send(const Frame * frame) {
        auto data = frame->dump();
        write_buf(data.data(), frame->get_len());
    }
};

static Connection *conn;

typedef struct {
    uint8_t start[2];
    uint8_t dst_addr;
    uint8_t src_addr;
    uint8_t command;
    uint8_t subcommand;
    uint8_t args[1500];
} frame_t;

static void set_vfo(void *arg) {
    if (!arg) {
        LV_LOG_ERROR("arg is NULL");
    }
    radio_vfo_t vfo = *(radio_vfo_t *)arg;
    cfg_sm.p_band_current_vfo.set(vfo);
}

static radio_mode_t ci_mode_2_x_mode(uint8_t mode, bool data_mode=false) {
    radio_mode_t r_mode;

    switch (mode) {
        case M_LSB:
            r_mode = data_mode ? radio_mode_lsb_dig : radio_mode_lsb;
            break;
        case M_USB:
            r_mode = data_mode ? radio_mode_usb_dig : radio_mode_usb;
            break;
        case M_AM:
            r_mode = radio_mode_am;
            break;
        case M_CW:
            r_mode = radio_mode_cw;
            break;
        case M_NFM:
            r_mode = radio_mode_nfm;
            break;
        case M_CWR:
            r_mode = radio_mode_cwr;
            break;
        default:
            break;
    }
    return r_mode;
}

/* 1A 06 set: data mode on/off keeps LSB/USB, only adds or drops the D */
static radio_mode_t ci_data_mode_2_x_mode(radio_mode_t x_mode, uint8_t data_mode) {
    if (data_mode) {
        if (x_mode == radio_mode_lsb) return radio_mode_lsb_dig;
        if (x_mode == radio_mode_usb) return radio_mode_usb_dig;
    } else {
        if (x_mode == radio_mode_lsb_dig) return radio_mode_lsb;
        if (x_mode == radio_mode_usb_dig) return radio_mode_usb;
    }
    return x_mode;
}

/* CW pitch: 0 = 300 Hz, 128 = 600 Hz, 255 = 900 Hz */
static uint8_t x_cw_pitch_2_ci(int32_t key_tone) {
    return static_cast<float>(key_tone - 300) * 0.425f;
}

static int32_t ci_cw_pitch_2_x(uint8_t val) {
    float tone = 2.3529411764705883f * static_cast<float>(val) + 300;
    return std::round(tone / 5.0f) * 5;     /* to 5 Hz */
}

/* 1A 01: ICOM band codes (BCD: 6 m is 0x10), 0x15 = outside the bands */
static uint8_t get_ci_band_id(int32_t freq) {
    switch (freq / 1000) {
        case 1800 ... 2000:   return 0x01;
        case 3400 ... 4100:   return 0x02;
        case 6900 ... 7500:   return 0x03;
        case 9900 ... 10500:  return 0x04;
        case 13900 ... 14500: return 0x05;
        case 17900 ... 18500: return 0x06;
        case 20900 ... 21500: return 0x07;
        case 24400 ... 25100: return 0x08;
        case 28000 ... 30000: return 0x09;
        case 50000 ... 54000: return 0x10;
    }
    return 0x15;
}

/* The band's centre frequency, 0 for an unknown code */
static int32_t ci_band_freq(uint8_t band) {
    switch (band) {
        case 0x01: return 1900000;
        case 0x02: return 3500000;
        case 0x03: return 7100000;
        case 0x04: return 10100000;
        case 0x05: return 14200000;
        case 0x06: return 18100000;
        case 0x07: return 21200000;
        case 0x08: return 24900000;
        case 0x09: return 28400000;
        case 0x10: return 50000000;
    }
    return 0;
}

static uint8_t x_mode_2_ci_mode(radio_mode_t mode, bool *data_mode=nullptr) {
    switch (mode) {
        case radio_mode_lsb_dig:
            if (data_mode) *data_mode = true;
        case radio_mode_lsb:
            return M_LSB;
            break;
        case radio_mode_usb_dig:
            if (data_mode) *data_mode = true;
        case radio_mode_usb:
            return M_USB;
            break;
        case radio_mode_cw:
            return M_CW;
            break;
        case radio_mode_cwr:
            return M_CWR;
            break;
        case radio_mode_am:
            return M_AM;
            break;
        case radio_mode_nfm:
            return M_NFM;
            break;
        default:
            return 0;
            break;
    }
}

/* CI-V filter width index, BCD encoded (index 10 is 0x10) */
static uint8_t get_if_bandwidth() {
    uint32_t bw = cfg_sm.cp_cur_filter_bw.get();
    uint8_t  val;

    switch (cfg_sm.cp_cur_mode.get()) {
        case radio_mode_cw:
        case radio_mode_cwr:
        case radio_mode_lsb:
        case radio_mode_lsb_dig:
        case radio_mode_usb:
        case radio_mode_usb_dig:
            if (bw <= 500) {
                val = (bw - 25) / 50;
            } else {
                val = (bw - 50) / 100 + 5;
            }
            break;
        case radio_mode_am:
        case radio_mode_nfm:
            val = (bw - 100) / 200;
            break;
        default:
            val = 31;
            break;
    }
    return (val / 10) << 4 | (val % 10);
}

static int32_t freq_step_from_ci(uint8_t val) {
    switch (val) {
        case 0x00:
            return 10;
        case 0x01:
            return 100;
        case 0x02:
            return 500;
        case 0x03:
            return 1000;
        case 0x04:
            return 5000;
    }
    return 500;

}

static uint8_t freq_step_to_ci(int32_t val) {
    switch (val) {
        case 1 ... 10:
            return 0x00;
        case 100:
            return 0x01;
        case 500:
            return 0x02;
        case 1000:
            return 0x03;
        case 5000:
            return 0x04;
    }
    return 0x02;
}

static void set_unsupported(const Frame *req, Frame *resp) {
    req->log("unsupported");
    resp->set_code(CODE_NG);
}

static Frame *process_req(const Frame *req) {
    auto resp = new Frame(req);

    int32_t        new_freq;
    radio_vfo_t    cur_vfo    = (radio_vfo_t)cfg_sm.p_band_current_vfo.get();
    int32_t        cur_freq   = cfg_sm.cp_fg_freq.get();
    radio_mode_t   cur_mode   = (radio_mode_t)cfg_sm.cp_cur_mode.get();
    radio_vfo_t    target_vfo = cur_vfo;
    uint8_t        vfo_id;

    size_t data_size = req->data.size();

    switch (req->command) {
        case C_SND_FREQ:
            if (data_size == 5) {
                cfg_sm.cp_fg_freq.set(from_bcd(req->data.data(), 10));
                resp->set_code(CODE_OK);
            } else {
                set_unsupported(req, resp);
            }
            break;

        case C_RD_FREQ:
            resp->set_payload_len(6);
            // bcd len - 5 bytes
            to_bcd(resp->data.data(), cur_freq, 10);
            break;

        case C_RD_MODE:
            {
                uint8_t v = x_mode_2_ci_mode(cur_mode);
                resp->set_payload_len(3);
                resp->data[0] = v;
                resp->data[1] = v;
            }
            break;

        case C_SET_FREQ:
            if (data_size == 5) {
                cfg_sm.cp_fg_freq.set(from_bcd(req->data.data(), 10));
                resp->set_code(CODE_OK);
            } else {
                set_unsupported(req, resp);
            }
            break;

        case C_SET_MODE:
            if ((data_size >= 1) && (data_size <= 2)) {
                cfg_sm.cp_cur_mode.set(ci_mode_2_x_mode(req->data[0]));
                if (data_size == 2) {
                    // filter selector -> req->data[1]
                }
                resp->set_code(CODE_OK);
            } else {
                set_unsupported(req, resp);
            }
            break;

        case C_SET_VFO:
            if (data_size == 1) {
                radio_vfo_t new_vfo;
                switch (req->data[0]) {
                    case S_VFOA:
                        if (cur_vfo != RADIO_VFO_A) {
                            new_vfo = RADIO_VFO_A;
                            cfg_sm.p_band_current_vfo.set(new_vfo);
                        }

                        resp->set_code(CODE_OK);
                        break;

                    case S_VFOB:
                        if (cur_vfo != RADIO_VFO_B) {
                            new_vfo = RADIO_VFO_B;
                            cfg_sm.p_band_current_vfo.set(new_vfo);
                        }
                        resp->set_code(CODE_OK);
                        break;

                    case S_XCHNG:
                        if (cur_vfo == RADIO_VFO_A) {
                            new_vfo = RADIO_VFO_B;
                        } else {
                            new_vfo = RADIO_VFO_A;
                        }
                        cfg_sm.p_band_current_vfo.set(new_vfo);
                        resp->set_code(CODE_OK);
                        break;

                    case S_BTOA:
                        cfg_sm.cfg_band_vfo_copy();
                        resp->set_code(CODE_OK);
                        break;

                    default:
                        set_unsupported(req, resp);
                        break;
                }
            } else if (data_size == 0) {
                resp->set_payload_len(2);
                resp->data[0] = cur_vfo == RADIO_VFO_A ? S_VFOA : S_VFOB;
            } else {
                set_unsupported(req, resp);
            }
            break;

        case C_CTL_SPLT:
            if (data_size == 0) {
                resp->set_payload_len(2);
                resp->data[0] = cfg_sm.p_band_split.get();
            } else if (data_size == 1) {
                cfg_sm.p_band_split.set(req->data[0]);
                resp->set_code(CODE_OK);
            } else {
                set_unsupported(req, resp);
            }
            break;

        case C_SET_TS:
            if (data_size == 0) {
                resp->set_payload_len(2);
                resp->data[0] = freq_step_to_ci(cfg_sm.p_mode_freq_step.get());
            } else if (data_size == 1) {
                cfg_sm.p_mode_freq_step.set(freq_step_from_ci(resp->data[0]));
                resp->set_code(CODE_OK);
            } else {
                set_unsupported(req, resp);
            }
            break;

        case C_CTL_ATT:
            if (data_size == 0) {
                resp->set_payload_len(2);
                resp->data[0] = cfg_sm.cp_cur_att.get() * 0x20;
            } else if (data_size == 1) {
                cfg_sm.cp_cur_att.set(req->data[0]);
                resp->set_code(CODE_OK);
            } else {
                set_unsupported(req, resp);
            }
            break;

        case C_CTL_LVL:
            if (data_size >= 1) {
                switch(req->data[0]) {
                    case 0x01:
                        // VOL
                        if (data_size == 1) {
                            resp->set_payload_len(4);
                            to_bcd_be(&resp->data[1], cfg_sm.p_volume.get() * 255 / 55, 3);
                        } else if (data_size == 3) {
                            cfg_sm.p_volume.set(from_bcd_be(&req->data[1], 3) * 55 / 255);
                        }
                        break;
                    case 0x02:
                        // RFG
                        if (data_size == 1) {
                            resp->set_payload_len(4);
                            to_bcd_be(&resp->data[1], cfg_sm.p_rfgain.get() * 255 / 100, 3);
                        } else if (data_size == 3) {
                            cfg_sm.p_rfgain.set(from_bcd_be(&req->data[1], 3) * 100 / 255);
                        }
                        break;
                    case 0x03:
                        // Squelch
                        if (data_size == 1) {
                            resp->set_payload_len(4);
                            to_bcd_be(&resp->data[1], cfg_sm.p_squelch.get() * 255 / 100, 3);
                        } else if (data_size == 3) {
                            cfg_sm.p_squelch.set(from_bcd_be(&req->data[1], 3) * 100 / 255);
                        }
                        break;
                    case 0x09:
                        // CW pitch
                        if (data_size == 1) {
                            resp->set_payload_len(4);
                            to_bcd_be(&resp->data[1], x_cw_pitch_2_ci(cfg_sm.p_key_tone.get()), 3);
                        } else if (data_size == 3) {
                            cfg_sm.p_key_tone.set(ci_cw_pitch_2_x(from_bcd_be(&req->data[1], 3)));
                        }
                        break;
                    case 0x0a:
                        // PWR
                        if (data_size == 1) {
                            resp->set_payload_len(4);
                            to_bcd_be(&resp->data[1], std::round(cfg_sm.p_pwr.get() * 255 / 10), 3);
                        } else if (data_size == 3) {
                            float pwr = from_bcd_be(&req->data[1], 3) * 10.0f / 255.0f;
                            pwr = LV_MIN(pwr, 10.0f);
                            cfg_sm.p_pwr.set(pwr);
                        }
                        break;
                    case 0x15:
                        // Monitor level
                        resp->set_payload_len(4);
                        to_bcd_be(&resp->data[1], cfg_sm.p_moni.get() * 255 / 100, 3);
                        break;
                    default:
                        set_unsupported(req, resp);
                        break;
                }
            } else {
                set_unsupported(req, resp);
            }
            break;

        case C_RD_SQSM:
            if (data_size == 1) {
                static float alc, pwr, swr;
                static uint8_t msg_id;
                tx_info_refresh(&msg_id, &alc, &pwr, &swr);
                uint8_t val;
                switch (req->data[0]) {
                    case 0x02: // S-meter
                        {
                            // Flrig
                            // 41 - S9
                            // 14 - S3
                            int16_t db = meter_get_raw_db();
                            val = db * 0.75f + 96;
                            resp->set_payload_len(4);
                            to_bcd_be(&resp->data[1], val, 3);
                        }
                        break;
                    case 0x11:  // Power
                        val = -pwr * pwr + 35 * pwr;
                        resp->set_payload_len(4);
                        to_bcd_be(&resp->data[1], val, 3);
                        break;
                    case 0x12:  // SWR
                        val = -21 * swr * swr + 134 * swr - 122;
                        resp->set_payload_len(4);
                        to_bcd_be(&resp->data[1], val, 3);
                        break;
                    case 0x13:  // ALC
                        val = alc * 120 / 10;
                        resp->set_payload_len(4);
                        to_bcd_be(&resp->data[1], val, 3);
                        break;
                    default:
                        resp->set_code(CODE_NG);
                        break;
                }
            } else {
                resp->set_code(CODE_NG);
            }
            break;

        case C_CTL_FUNC:
            if ((data_size == 1) || (data_size == 2)) {
                switch (req->data[0]) {
                    case 0x02:
                        // PRE
                        if (data_size == 1) {
                            resp->set_payload_len(3);
                            resp->data[1] = cfg_sm.cp_cur_pre.get();
                        } else {
                            cfg_sm.cp_cur_pre.set(req->data[1] > 0);
                            resp->set_code(CODE_OK);
                        }
                        break;
                    case 0x22:
                        // NB
                        if (data_size == 1) {
                            resp->set_payload_len(3);
                            resp->data[1] = cfg_sm.p_nb.get();
                        } else {
                            cfg_sm.p_nb.set(req->data[1]);
                            resp->set_code(CODE_OK);
                        }
                        break;
                    case 0x40:
                        // NR
                        if (data_size == 1) {
                            resp->set_payload_len(3);
                            resp->data[1] = cfg_sm.p_nr.get();
                        } else {
                            cfg_sm.p_nr.set(req->data[1]);
                            resp->set_code(CODE_OK);
                        }
                        break;
                    case 0x44:
                        // COMP
                        if (data_size == 1) {
                            resp->set_payload_len(3);
                            resp->data[1] = 0x00;
                        } else {
                            // COMP set is not suported yet
                            resp->set_code(CODE_OK);
                        }
                        break;
                    case 0x45:
                        // Monitor [MONI] On/off
                        resp->set_code(CODE_NG);
                        break;
                    case 0x46:
                        // VOX
                        if (data_size == 1) {
                            resp->set_payload_len(3);
                            resp->data[1] = 0x00;
                        } else {
                            // VOX set is not suported yet
                            resp->set_code(CODE_OK);
                        }
                        break;
                    case 0x5D:
                        // Tone squelch function
                        resp->set_code(CODE_NG);
                        break;
                    default:
                        set_unsupported(req, resp);
                }
            } else {
                set_unsupported(req, resp);
            }
            break;

        case C_RD_TRXID:
            if ((data_size == 1) && (req->data[0] == 0)) {
                resp->set_payload_len(3);
                resp->data[1] = 0xA4;
            }
            break;

        case C_CTL_MEM:
            // TODO: Implement another options
            if (data_size == 1) {
                switch (req->data[0]) {
                    case MEM_BS_REG:
                        resp->set_payload_len(4);
                        resp->data[1] = get_ci_band_id(cfg_sm.cp_fg_freq.get());
                        resp->data[2] = 0x02;
                        break;

                    case MEM_IF_FW:
                        resp->set_payload_len(3);
                        resp->data[1] = get_if_bandwidth();
                        break;

                    case MEM_DM_FG:
                        // data mode, filter
                        resp->set_payload_len(4);
                        resp->data[1] = (cur_mode == radio_mode_lsb_dig) || (cur_mode == radio_mode_usb_dig);
                        resp->data[2] = 0x01;
                        break;

                    default:
                        set_unsupported(req, resp);
                        break;
                }
            } else {
                switch (req->data[0]) {
                    case MEM_BS_REG:
                        {
                            int32_t freq = ci_band_freq(req->data[1]);
                            if (freq) {
                                cfg_sm.cp_fg_freq.set(freq);
                            }
                            resp->set_code(freq ? CODE_OK : CODE_NG);
                        }
                        break;
                    case MEM_LOCK:  // Various controls for icom, unsupported
                        resp->set_code(CODE_NG);
                        break;
                    case MEM_DM_FG:
                        // data mode, filter
                        cfg_sm.cp_cur_mode.set(ci_data_mode_2_x_mode(cur_mode, req->data[1]));
                        resp->set_code(CODE_OK);
                        break;
                    default:
                        set_unsupported(req, resp);
                        break;
                }
            }
            break;

        case C_CTL_PTT:
            if ((data_size >= 1) && (req->data[0] == 0x00)) {
                if (data_size == 1) {
                    resp->set_payload_len(3);
                    resp->data[1] = (radio_get_state() == RADIO_RX) ? 0 : 1;
                } else {
                    switch (req->data[1]) {
                        case 0:
                            radio_set_ptt(false);
                            break;

                        case 1:
                            radio_set_ptt(true);
                            break;
                    }
                    resp->set_payload_len(3);
                    resp->data[1] = CODE_OK;
                }
            }
            break;

        case C_CTL_RIT:
            /* 21 00 offset (2 BCD bytes Hz + sign), 21 01 RIT on/off, 21 02 XIT on/off. Off = offset 0.
             * OmniRig polls 21 01/02 every cycle: an NG there makes it time out and drop the rig. */
            if (data_size >= 1 && req->data[0] == 0x00) {
                if (data_size == 1) {
                    int32_t rit = cfg_sm.p_rit.get();
                    resp->set_payload_len(5);
                    to_bcd(resp->data.data() + 1, rit < 0 ? -rit : rit, 4);
                    resp->data[3] = rit < 0 ? 1 : 0;
                } else if (data_size == 4) {
                    int32_t hz = from_bcd(req->data.data() + 1, 4);
                    cfg_sm.p_rit.set(req->data[3] ? -hz : hz);
                    resp->set_code(CODE_OK);
                } else {
                    set_unsupported(req, resp);
                }
            } else if (data_size >= 1 && (req->data[0] == 0x01 || req->data[0] == 0x02)) {
                auto &p = req->data[0] == 0x01 ? cfg_sm.p_rit : cfg_sm.p_xit;
                if (data_size == 1) {
                    resp->set_payload_len(3);
                    resp->data[1] = p.get() != 0;
                } else {
                    if (req->data[1] == 0)
                        p.set(0);
                    resp->set_code(CODE_OK);
                }
            } else {
                set_unsupported(req, resp);
            }
            break;

        case C_SEND_SEL_FREQ:
            {
                ComputedParameter<int32_t> *freq;
                if (req->data[0] == 0) {
                    // fg freq
                    freq = &cfg_sm.cp_fg_freq;
                } else {
                    // bg freq
                    freq = &cfg_sm.cp_bg_freq;
                }
                if (data_size == 1) {
                    resp->set_payload_len(7);
                    to_bcd(&resp->data[1], freq->get(), 10);
                } else if (data_size == 6) {
                    freq->set(from_bcd(&req->data[1], 10));
                    resp->set_code(CODE_OK);
                } else {
                    set_unsupported(req, resp);
                }
            }
            break;

        case C_SEND_SEL_MODE:
            {
                uint8_t v;
                radio_mode_t new_mode;
                bool data_mode = false;
                Parameter<int32_t> *mode_par;

                if (req->data[0] == 0) {
                    // fg
                    mode_par = cfg_sm.p_band_current_vfo.get() == RADIO_VFO_A ? &cfg_sm.p_band_vfoa_mode :&cfg_sm.p_band_vfob_mode;
                } else {
                    // bg
                    mode_par = cfg_sm.p_band_current_vfo.get() == RADIO_VFO_B ? &cfg_sm.p_band_vfoa_mode :&cfg_sm.p_band_vfob_mode;
                }
                switch (data_size) {
                    case 1:
                        // Read command
                        v = x_mode_2_ci_mode((radio_mode_t)mode_par->get(), &data_mode);
                        resp->set_payload_len(5);
                        resp->data[1] = v;
                        resp->data[2] = data_mode;
                        // filter
                        resp->data[3] = 1;
                        break;

                    case 4:
                        // Write command with filter byte
                    case 3:
                        // Write command with data byte
                        data_mode = req->data[2];
                    case 2:
                        // Write command
                        new_mode = ci_mode_2_x_mode(req->data[1], data_mode);
                        mode_par->set(new_mode);
                        resp->set_code(CODE_OK);
                        break;
                    default:
                        set_unsupported(req, resp);
                        break;
                }

            }
            break;

        case C_CTL_SCP:
            if (data_size >= 1) {
                switch (req->data[0]) {
                    case 0x10:  // Send/read the Scope ON/OFF status
                        if (data_size == 1) {
                            resp->set_payload_len(3);
                            resp->data[1] = 1;
                        } else {
                            resp->set_payload_len(2);
                        }
                        break;
                    case 0x11:  // Send/read the Scope wave data output: none on serial and Bluetooth
                        if (data_size == 1) {
                            resp->set_payload_len(3);
                            resp->data[1] = 0;
                        } else {
                            resp->set_payload_len(2);
                        }
                        break;
                    case 0x13:  // Single/Dual scope setting
                        resp->set_code(CODE_NG);
                        break;
                    case 0x14:  // Send/read the Scope Center mode or Fixed mode setting
                        if (data_size == 1) {
                            // Report center mode
                            resp->set_payload_len(4);
                            resp->data[1] = 0;
                            resp->data[2] = 0;
                        } else {
                            resp->set_payload_len(2);
                        }
                        break;
                    case 0x15:  // Scope span settings: half span, follows the radio zoom
                        if (data_size <= 2) {
                            resp->set_payload_len(8);
                            to_bcd(&resp->data[1] + 1, 50000 / cfg_sm.p_mode_zoom.get(), 10);
                        } else if (data_size >= 7) {
                            int32_t span = from_bcd(&req->data[2], 10);
                            int32_t zoom = 1;
                            while (zoom < 8 && 50000 / (zoom * 2) >= span)
                                zoom *= 2;
                            cfg_sm.p_mode_zoom.set(zoom);
                            resp->set_code(CODE_OK);
                        } else {
                            resp->set_code(CODE_NG);
                        }
                        break;
                    case 0x17:  // Scope hold function
                        resp->set_code(CODE_NG);
                        break;
                    case 0x19:
                        resp->set_payload_len(6);
                        resp->data[1] = 0;
                        resp->data[2] = 0;
                        resp->data[3] = 0;
                        resp->data[4] = 0;
                        break;
                    case 0x1A:
                        // Sweep speed setting
                        resp->set_code(CODE_NG);
                        break;
                    default:
                        set_unsupported(req, resp);
                        break;
                }
            } else {
                set_unsupported(req, resp);
            }
            break;

        default:
            set_unsupported(req, resp);
            break;
    }
    return resp;
}

/* A request is handled on the GUI thread: it reads and sets the parameters, whose observers
 * drive LVGL, and the GUI thread owns both (audit A05). The CAT thread only moves bytes */
struct req_job_t {
    const Frame *req;
    Frame       *resp;
};

static void process_req_cb(void *arg) {
    auto *job = static_cast<req_job_t *>(arg);
    job->resp = process_req(job->req);
}

static void cat_thread() {
    pthread_setname_np(pthread_self(), "cat");
    while (true) {
        bool sleep = true;

        const Frame *req = conn->feed();
        if (req) {
            sleep = false;
            conn->send(req);
            req_job_t job = {req, nullptr};
            if (!scheduler_call(process_req_cb, &job)) {
                job.resp = new Frame(req);
                job.resp->set_code(CODE_NG);
            }
            // job.resp->log("resp");
            conn->send(job.resp);
            delete job.resp;
            delete req;
        }

        while (!send_queue.empty()) {
            sleep = false;
            auto data = send_queue.pop();
            conn->send(data.data(), data.size());
        }
        if (sleep) {
            using namespace std::chrono_literals;
            std::this_thread::sleep_for(10ms);
        }
    }
}

void cat_init() {
    /* UART */
    radio_usb_to_cat();

    /* X6100_CAT_DEV: another port, for a test through a pty */
    const char *dev = getenv("X6100_CAT_DEV");
    int fd = open(dev ? dev : BOARD_CAT_DEV, O_RDWR | O_NONBLOCK | O_NOCTTY);

    if (fd > 0) {
        struct termios attr;

        tcgetattr(fd, &attr);

        cfsetispeed(&attr, B19200);
        cfsetospeed(&attr, B19200);
        // cfsetispeed(&attr, B115200);
        // cfsetospeed(&attr, B115200);
        cfmakeraw(&attr);

        if (tcsetattr(fd, 0, &attr) < 0) {
            close(fd);
            LV_LOG_ERROR("UART set speed");
        }
    } else {
        LV_LOG_ERROR("UART open");
    }

    conn = new Connection(fd);

    cfg_sm.cp_fg_freq.subscribe(on_fg_freq_change, &cfg_sm.cp_fg_freq);
    cfg_sm.cp_cur_mode.subscribe(on_mode_change, nullptr);
    cfg_sm.p_band_current_vfo.subscribe(on_vfo_change, nullptr);

    /* * */
    std::thread thread(cat_thread);
    thread.detach();
}

/* Unsolicited (transceive) frame to the serial CAT */
static void send_notify(const Frame &frame) {
    send_queue.push(frame.dump());
}

static void on_fg_freq_change(Subject *s, void *user_data) {
    auto *p = static_cast<ComputedParameter<int32_t>*>(user_data);
    int32_t new_freq = p->get();
    Frame frame{0, LOCAL_ADDRESS, C_SND_FREQ};
    // bcd len - 5 bytes
    frame.set_payload_len(6);
    to_bcd(frame.data.data(), new_freq, 10);
    send_notify(frame);
}

/* Transceive: mode (01) and VFO (07) changes too, from Dyuldin's upstream 6886aa36 */
static void on_mode_change(Subject *s, void *user_data) {
    Frame frame{0, LOCAL_ADDRESS, C_SND_MODE};
    frame.set_payload_len(2);
    frame.data[0] = x_mode_2_ci_mode((radio_mode_t)cfg_sm.cp_cur_mode.get());
    send_notify(frame);
}

static void on_vfo_change(Subject *s, void *user_data) {
    Frame frame{0, LOCAL_ADDRESS, C_SET_VFO};
    frame.set_payload_len(2);
    frame.data[0] = cfg_sm.p_band_current_vfo.get() == RADIO_VFO_A ? S_VFOA : S_VFOB;
    send_notify(frame);
}
