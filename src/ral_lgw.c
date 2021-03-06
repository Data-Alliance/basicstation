/*
 *  --- Revised 3-Clause BSD License ---
 *  Copyright (C) 2016-2019, SEMTECH (International) AG.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without modification,
 *  are permitted provided that the following conditions are met:
 *
 *      * Redistributions of source code must retain the above copyright notice,
 *        this list of conditions and the following disclaimer.
 *      * Redistributions in binary form must reproduce the above copyright notice,
 *        this list of conditions and the following disclaimer in the documentation
 *        and/or other materials provided with the distribution.
 *      * Neither the name of the copyright holder nor the names of its contributors
 *        may be used to endorse or promote products derived from this software
 *        without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 *  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL SEMTECH BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 *  OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 *  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#if defined(CFG_lgw1)

#if (!defined(CFG_ral_lgw) && !defined(CFG_ral_master_slave)) || (defined(CFG_ral_lgw) && defined(CFG_ral_master_slave))
#error Exactly one of the two params must be set: CFG_ral_lgw CFG_ral_master_slave
#endif

#include "s2conf.h"
#include "tc.h"
#include "timesync.h"
#include "sys.h"
#include "sx1301conf.h"
#include "ral.h"
#include "lgw/loragw_reg.h"
#include "lgw/loragw_hal.h"

#define RAL_MAX_RXBURST 10

#define FSK_BAUD      50000
#define FSK_FDEV      25  // [kHz]
#define FSK_PRMBL_LEN 5

static const u2_t SF_MAP[] = {
    [SF12 ]= DR_LORA_SF12,
    [SF11 ]= DR_LORA_SF11,
    [SF10 ]= DR_LORA_SF10,
    [SF9  ]= DR_LORA_SF9,
    [SF8  ]= DR_LORA_SF8,
    [SF7  ]= DR_LORA_SF7,
    [FSK  ]= DR_UNDEFINED,
    [SFNIL]= DR_UNDEFINED,
};

static const u1_t BW_MAP[] = {
    [BW125]= BW_125KHZ,
    [BW250]= BW_250KHZ,
    [BW500]= BW_500KHZ,
    [BWNIL]= BW_UNDEFINED
};


static int to_sf (int lgw_sf) {
    for( u1_t sf=SF12; sf<=FSK; sf++ )
        if( SF_MAP[sf] == lgw_sf )
            return sf;
    return SFNIL;
}

static int to_bw (int lgw_bw) {
    for( u1_t bw=BW125; bw<=BW500; bw++ )
        if( BW_MAP[bw] == lgw_bw )
            return bw;
    return BWNIL;
}


rps_t ral_lgw2rps (struct lgw_pkt_rx_s* p) {
    return p->modulation == MOD_LORA
        ? rps_make(to_sf(p->datarate), to_bw(p->bandwidth))
        : FSK;
}


void ral_rps2lgw (rps_t rps, struct lgw_pkt_tx_s* p) {
    assert(rps != RPS_ILLEGAL);
    if( rps_sf(rps) == FSK ) {
        p->modulation = MOD_FSK;
        p->datarate   = FSK_BAUD;
        p->f_dev      = FSK_FDEV;
        p->preamble   = FSK_PRMBL_LEN;
    } else {
        p->modulation = MOD_LORA;
        p->datarate   = SF_MAP[rps_sf(rps)];
        p->bandwidth  = BW_MAP[rps_bw(rps)];;
    }
}

// Make a clock sync measurement:
//  pps_en      w/o checking on latches PPS xticks
//  last_xtime  read and update last xticks to form a continuous 64 bit time
//  timesync    return measured data - isochronous times for MCU/SX1301 and opt. latched PPS
// Return:
//  Measure of quality (absolute value) - caller uses this to weed out bad measurements
//  In this impl. we return the time the measurement took - smallest values are best values
//
int ral_getTimesync (u1_t pps_en, sL_t* last_xtime, timesync_t* timesync) {
    u4_t pps_xticks;
    if( pps_en ) {
        // First read last latched value - interval between time syncs needs to be >1s so that a PPS could have happened.
        // Read last latched value - when PPS occurred. If no PPS happened this returns
        // the time when LGW_GPS_EN was set to 1.
        lgw_get_trigcnt(&pps_xticks);
        lgw_reg_w(LGW_GPS_EN, 0);       // PPS latch holds current
    }
    ustime_t t0 = rt_getTime();
    u4_t xticks = 0;
    lgw_get_trigcnt(&xticks);
    ustime_t t1 = rt_getTime();
    sL_t d = (s4_t)(xticks - *last_xtime);
    if( d < 0 ) {
        LOG(MOD_SYN|CRITICAL, "SX1301 time sync roll over - no update for a long time!");
        d += (sL_t)1<<32;
    }
    timesync->xtime = *last_xtime += d;
    timesync->ustime = (t0+t1)/2;
    if( pps_en ) {
        // PPS latch will hold now current xticks
        lgw_reg_w(LGW_GPS_EN, 1);
        timesync->pps_xtime = timesync->xtime + (s4_t)(pps_xticks - xticks);
    } else {
        // Signal no PPS
        timesync->pps_xtime = 0;
    }
    return (int)(t1-t0);
}

#if defined(CFG_ral_lgw)
static u1_t       pps_en;
static s2_t       txpowAdjust;    // scaled by TXPOW_SCALE
static sL_t       last_xtime;
static tmr_t      rxpollTmr;
static tmr_t      syncTmr;

#if defined(CFG_testpin)
static rps_t      testpin_rps;
static u1_t       testpin_mode;
static sL_t       testpin_xtime_beg;
static sL_t       testpin_xtime_end;
#endif // defined(CFG_testpin)


// ATTR_FASTCODE 
static void synctime (tmr_t* tmr) {
    timesync_t timesync;
#if defined(CFG_testpin)
    sL_t last_xtime_bak = last_xtime;
#endif
    int quality = ral_getTimesync(pps_en, &last_xtime, &timesync);
#if defined(CFG_testpin)
    if( sys_modePPS == PPS_TESTPIN ) {
        // The PPS pin is not a 1Hz pulse used for time sync
        // but is raised by a device under test to signal the time
        // of certain operations (TX end, start of RX)
        sL_t pps_xtime = timesync.pps_xtime;
        sL_t d = pps_xtime - last_xtime_bak;
        if( d > 100 && TC && testpin_mode ) {
            // PPS latch is not filled with last enabling time.
            // A testpin edge seems to have happend
            ujbuf_t sendbuf = (TC->s2ctx.getSendbuf)(&TC->s2ctx, MIN_UPJSON_SIZE);
            if( sendbuf.buf != NULL ) {
                // Websocket has space
                LOG(MOD_RAL|WARNING, "Testpin mode - latched %s PPS @%R: %lX vs [%lX..%lX]",
                    testpin_mode==1 ? "DN":"UP", testpin_rps,
                    pps_xtime, testpin_xtime_beg, testpin_xtime_end);
                uj_encOpen(&sendbuf, '{');
                uj_encKVn(&sendbuf,
                          "msgtype",   's', "testpin",
                          "mode",      's', testpin_mode==1 ? "dn" : "up",
                          "sf",        'i', 12 - rps_sf(testpin_rps),
                          "bw",        'i', 125 * (1<<rps_bw(testpin_rps)),
                          "xtime_pin", 'I', pps_xtime,
                          "xtime_beg", 'I', testpin_xtime_beg,
                          "xtime_end", 'I', testpin_xtime_end,
                          NULL);
                uj_encClose(&sendbuf, '}');
                (*TC->s2ctx.sendText)(&TC->s2ctx, &sendbuf);
                testpin_mode = 0;
            }
        }
        // Clear - testpin mode is not a PPS pulse
        timesync.pps_xtime = 0;
    }
#endif // defined(CFG_testpin)
    ustime_t delay = ts_updateTimesync(0, quality, &timesync);
    rt_setTimer(&syncTmr, rt_micros_ahead(delay));
}

u1_t ral_altAntennas (u1_t txunit) {
    return 0;
}


int ral_tx (txjob_t* txjob, s2ctx_t* s2ctx, int nocca) {
    struct lgw_pkt_tx_s pkt_tx;
    memset(&pkt_tx, 0, sizeof(pkt_tx));

    if( txjob->txflags & TXFLAG_BCN ) {
        pkt_tx.tx_mode = ON_GPS;
        pkt_tx.preamble = 10;
    } else {
        pkt_tx.tx_mode = TIMESTAMPED;
        pkt_tx.preamble = 8;
    }
    rps_t rps = s2e_dr2rps(s2ctx, txjob->dr);
    ral_rps2lgw(rps, &pkt_tx);
    pkt_tx.freq_hz    = txjob->freq;
    pkt_tx.count_us   = txjob->xtime;
    pkt_tx.rf_chain   = 0;
    pkt_tx.rf_power   = (float)(txjob->txpow - txpowAdjust) / TXPOW_SCALE;
    pkt_tx.coderate   = CR_LORA_4_5;
    pkt_tx.invert_pol = true;
    pkt_tx.no_crc     = true;
    pkt_tx.no_header  = false;
    pkt_tx.size = txjob->len;
    memcpy(pkt_tx.payload, &s2ctx->txq.txdata[txjob->off], pkt_tx.size);

    // NOTE: nocca not possible to implement with current libloragw API
    int err = lgw_send(pkt_tx);
    if( err != LGW_HAL_SUCCESS ) {
        if( err != LGW_LBT_ISSUE ) {
            LOG(MOD_RAL|ERROR, "lgw_send failed");
            return RAL_TX_FAIL;
        }
        return RAL_TX_NOCA;
    }
#if defined(CFG_testpin)
    // If testpin mode and we have an UP data frame addressing LWTESTAPP port
    if( sys_modePPS == PPS_TESTPIN && pkt_tx.size >= 13 &&
        ((pkt_tx.payload[0] & 0xE0) == 0x60 || (pkt_tx.payload[0] & 0xE0) == 0xA0) &&
            pkt_tx.size >= 13 + (pkt_tx.payload[5] & 0xF) &&
            pkt_tx.payload[8+(pkt_tx.payload[5] & 0xF)] == 224 ) {
        // then trigger time sync some while after TX has ended.
        // Thereby reading PPS register which should have recorded testpin edge set by device.
        ustime_t airtime = s2e_calcDnAirTime(rps, pkt_tx.size);
        testpin_xtime_beg = txjob->xtime;
        testpin_xtime_end = txjob->xtime + airtime;
        testpin_rps = rps;
        testpin_mode = 1;  // DN
        rt_setTimer(&syncTmr, rt_millis_ahead(200)+airtime);  // after TX has ended
        LOG(MOD_RAL|WARNING, "Testpin mode - TX frame @%R: %lu/%lX .. %lu/%lX",
            rps, testpin_xtime_beg, testpin_xtime_beg, testpin_xtime_end, testpin_xtime_end);
    }
#endif // defined(CFG_testpin)
    return RAL_TX_OK;
}


int ral_txstatus (u1_t txunit) {
    u1_t status;
    int err = lgw_status(TX_STATUS, &status);
    if (err != LGW_HAL_SUCCESS) {
        LOG(MOD_RAL|ERROR, "lgw_status failed");
        return TXSTATUS_IDLE;
    }
    if( status == TX_SCHEDULED )
        return TXSTATUS_SCHEDULED;
    if( status == TX_EMITTING )
        return TXSTATUS_EMITTING;
    return TXSTATUS_IDLE;
}


void ral_txabort (u1_t txunit) {
    lgw_abort_tx();
}

//ATTR_FASTCODE 
static void rxpolling (tmr_t* tmr) {
    int rounds = 0;
    while(rounds++ < RAL_MAX_RXBURST) {
        struct lgw_pkt_rx_s pkt_rx;
        int n = lgw_receive(1, &pkt_rx);
        if( n < 0 || n > 1 ) {
            LOG(MOD_RAL|ERROR, "lgw_receive error: %d", n);
            break;
        }
        if( n==0 ) {
            break;
        }
        LOG(XDEBUG, "RX mod=%s f=%d bw=%d sz=%d dr=%d %H", pkt_rx.modulation == 0x10 ? "LORA" : "FSK", pkt_rx.freq_hz, (int[]){0,500,250,125}[pkt_rx.bandwidth], pkt_rx.size, pkt_rx.datarate, pkt_rx.size, pkt_rx.payload);

        rxjob_t* rxjob = !TC ? NULL : s2e_nextRxjob(&TC->s2ctx);
        if( rxjob == NULL ) {
            LOG(ERROR, "SX1301 RX frame dropped - out of space");
            break; // Allow to flush RX jobs
        }
        if( pkt_rx.status != STAT_CRC_OK ) {
            LOG(XDEBUG, "Dropped frame without CRC or with broken CRC");
            continue; // silently ignore bad CRC
        }
        if( pkt_rx.size > MAX_RXFRAME_LEN ) {
            // This should not happen since caller provides
            // space for max frame length - 255 bytes
            LOG(MOD_RAL|ERROR, "Frame size (%d) exceeds offered buffer (%d)", pkt_rx.size, MAX_RXFRAME_LEN);
            continue;
        }
        memcpy(&TC->s2ctx.rxq.rxdata[rxjob->off], pkt_rx.payload, pkt_rx.size);
        rxjob->len   = pkt_rx.size;
        rxjob->freq  = pkt_rx.freq_hz;
        rxjob->xtime = ts_xticks2xtime(pkt_rx.count_us, last_xtime);
        rxjob->rssi  = (u1_t)-pkt_rx.rssi;
        rxjob->snr   = (s1_t)(pkt_rx.snr*8);
        rps_t rps = ral_lgw2rps(&pkt_rx);
        rxjob->dr = s2e_rps2dr(&TC->s2ctx, rps);
        if( rxjob->dr == DR_ILLEGAL ) {
            LOG(MOD_RAL|ERROR, "Unable to map to an up DR: %R", rps);
            continue;
        }
        s2e_addRxjob(&TC->s2ctx, rxjob);

#if defined(CFG_testpin)
        // If testpin mode and we have an UP data frame addressing LWTESTAPP port
        if( sys_modePPS == PPS_TESTPIN && pkt_rx.size >= 13 &&
            ((pkt_rx.payload[0] & 0xE0) == 0x40 || (pkt_rx.payload[0] & 0xE0) == 0x80) &&
            pkt_rx.size >= 13 + (pkt_rx.payload[5] & 0xF) &&
            pkt_rx.payload[8+(pkt_rx.payload[5] & 0xF)] == 224 ) {
            // then trigger time sync after a while.
            // Thereby reading PPS register which should have recorded testpin edge set by device.
            testpin_xtime_end = rxjob->xtime;
            testpin_xtime_beg = rxjob->xtime - s2e_calcUpAirTime(rps, pkt_rx.size);
            testpin_rps = rps;
            testpin_mode = 2;   // UP
            rt_setTimer(&syncTmr, rt_millis_ahead(200));
            LOG(MOD_RAL|WARNING, "Testpin mode - UP frame @ %R: %lu/%lX..%lu/%lX",
                rps, testpin_xtime_beg, testpin_xtime_beg, testpin_xtime_end, testpin_xtime_end);
        }
#endif // defined(CFG_testpin)
    }
    s2e_flushRxjobs(&TC->s2ctx);
    rt_setTimer(tmr, rt_micros_ahead(RX_POLL_INTV));
}


int ral_config (str_t hwspec, u4_t cca_region, char* json, int jsonlen) {
    if( strcmp(hwspec, "sx1301/1") != 0 ) {
        LOG(ERROR, "Unsupported hwspec=%s", hwspec);
        return 0;
    }
    ujdec_t D;
    uj_iniDecoder(&D, json, jsonlen);
    if( uj_decode(&D) ) {
        LOG(ERROR, "Parsing of sx1301 channel setup JSON failed");
        return 0;
    }
    if( uj_null(&D) ) {
        LOG(ERROR, "sx1301_conf is null but a hw setup IS required - no fallbacks");
        return 0;
    }
    uj_enterArray(&D);
    int ok=0, slaveIdx;
    while( (slaveIdx = uj_nextSlot(&D)) >= 0 ) {
        dbuf_t json = uj_skipValue(&D);
        if( slaveIdx == 0 ) {
            struct sx1301conf sx1301conf;

            if( sx1301conf_parse_setup(&sx1301conf, -1, hwspec, json.buf, json.bufsize) &&
                sys_runRadioInit(sx1301conf.device) &&
                sx1301conf_start(&sx1301conf, cca_region) ) {
                // Radio started
                txpowAdjust = sx1301conf.txpowAdjust;
                pps_en = sx1301conf.pps;
                last_xtime = ts_newXtimeSession(0);
                rt_yieldTo(&rxpollTmr, rxpolling);
                rt_yieldTo(&syncTmr, synctime);
                ok = 1;
            }
        }
    }
    uj_exitArray(&D);
    uj_assertEOF(&D);
    return ok;
}


// Lora gateway library is run locally - no subprocesses needed.
void ral_ini() {
    last_xtime = 0;
    rt_iniTimer(&rxpollTmr, rxpolling);
    rt_iniTimer(&syncTmr, synctime);
}

void ral_stop() {
    lgw_stop();
    last_xtime = 0;
    rt_clrTimer(&rxpollTmr);
    rt_clrTimer(&syncTmr);
}

#endif // defined(CFG_ral_lgw)
#endif // defined(CFG_lgw1)
