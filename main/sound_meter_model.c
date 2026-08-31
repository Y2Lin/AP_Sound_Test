// main/sound_meter_model.c -- 纯逻辑实现,全部整数运算,可在主机测试。
// 数学路线:dBFS*10 = 200*log10(rms/32768) = 60.206*(log2(rms) - 15)。
// log2 拆成"整数位 n + 折线查表的小数位",表是 log2(1+k/16) 的 Q16 定点值,
// 段内线性插值,整体误差远小于 1(即 0.1 dB)。
#include "sound_meter_model.h"

// 满幅参考:200*log10(32768) = 903.09,取 903(静音/1 LSB 的 dBFS 下限取负值)。
#define DBFS_REF_X10     903
// dBFS*10 = 60206/1000 * (log2 - 15):分子分母同乘 1000 消掉小数。
#define DBFS_LOG2_NUM    60206
#define DBFS_LOG2_DEN    1000

// log2(1+k/16) * 65536 四舍五入取整(k=0..16,k=16 即 log2(2)=1)。
static const uint32_t LOG2_FRAC_Q16[17] = {
    0, 5732, 11136, 16248, 21098, 25711, 30109, 34312,
    38336, 42196, 45904, 49472, 52911, 56229, 59434, 62534, 65536,
};

uint32_t sound_meter_isqrt_u64(uint64_t v) {
    if (v == 0) return 0;
    // 结果上界:sqrt(2^64-1) < 2^32,取 0xFFFFFFFF 可保证 mid*mid 不溢出。
    uint64_t lo = 0, hi = 0xFFFFFFFFu;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo + 1) / 2;   // 上取整,保证循环能推进
        if (mid * mid <= v) lo = mid;
        else                hi = mid - 1;
    }
    return (uint32_t)lo;
}

// 四舍五入的有符号除法(对负数同样按"最近"取整,C 的截断会引入 0.5 偏差)。
static int32_t rounded_div_i64(int64_t num, int64_t den) {
    if (num >= 0) return (int32_t)((num + den / 2) / den);
    return (int32_t)((num - den / 2) / den);
}

int32_t sound_meter_dbfs_x10(uint32_t rms) {
    if (rms == 0) return -DBFS_REF_X10;
    if (rms > 32767u) rms = 32767u;    // 防御:16bit 样本正常不会超过

    int n = 31 - __builtin_clz(rms);   // rms ∈ [2^n, 2^(n+1)),n <= 14
    // 小数部分 frac = (rms - 2^n)/2^n,放大到 Q16;rms<=32767 保证不溢出。
    uint32_t frac_q16 = ((rms - (1u << n)) << 16) >> n;
    uint32_t k   = frac_q16 >> 12;     // 16 段折线的段号 0..15
    uint32_t rem = frac_q16 & 0xFFFu;  // 段内位置(Q12)
    uint32_t lo_v = LOG2_FRAC_Q16[k];
    uint32_t hi_v = LOG2_FRAC_Q16[k + 1];
    uint32_t frac_log2_q16 = lo_v + (((hi_v - lo_v) * rem) >> 12);

    uint32_t log2_q16 = ((uint32_t)n << 16) + frac_log2_q16;
    // dBFS*10 = 60206/1000 * (log2_q16/65536 - 15)
    // 注意差值必须按有符号计算:uint32 直接相减会在 rms<2^15 时下溢成大正数。
    int64_t scaled = (int64_t)DBFS_LOG2_NUM *
                     ((int64_t)log2_q16 - (int64_t)(15u << 16));
    return rounded_div_i64(scaled, (int64_t)DBFS_LOG2_DEN * 65536);
}

int32_t sound_meter_display_db_x10(uint32_t rms) {
    int32_t db = sound_meter_dbfs_x10(rms) + SOUND_METER_DB_OFFSET_X10;
    if (db < 0)   return 0;
    if (db > 1200) return 1200;
    return db;
}

int32_t sound_meter_bar_pct(int32_t db_x10) {
    if (db_x10 <= 300) return 0;
    if (db_x10 >= 900) return 100;
    return (db_x10 - 300) / 6;    // (db-300)*100/600 的整数等价
}

void sound_meter_model_init(sound_meter_model_t *m, int32_t thr_db) {
    m->threshold_db = sound_meter_threshold_load(thr_db);
    m->last_db_x10  = 0;
    m->peak_db_x10  = 0;
    m->db_sum_x10   = 0;
    m->frames       = 0;
    m->alarm_count  = 0;
    m->alarm_active = false;
    m->over_frames  = 0;
    m->under_frames = 0;
}

int32_t sound_meter_threshold_step(int32_t thr_db, int steps_db) {
    int32_t v = thr_db + steps_db;
    if (v < SOUND_METER_THR_MIN_DB) return SOUND_METER_THR_MIN_DB;
    if (v > SOUND_METER_THR_MAX_DB) return SOUND_METER_THR_MAX_DB;
    return v;
}

int32_t sound_meter_threshold_load(int32_t stored_db) {
    if (stored_db < SOUND_METER_THR_MIN_DB ||
        stored_db > SOUND_METER_THR_MAX_DB) {
        return SOUND_METER_THR_DEFAULT_DB;
    }
    return stored_db;
}

bool sound_meter_model_frame(sound_meter_model_t *m, uint32_t rms) {
    int32_t db = sound_meter_display_db_x10(rms);
    m->last_db_x10 = db;
    m->frames++;
    m->db_sum_x10 += db;
    if (db > m->peak_db_x10) m->peak_db_x10 = db;

    int32_t thr_x10    = m->threshold_db * 10;
    int32_t release_x10 = thr_x10 - SOUND_METER_HYST_DB * 10;
    if (db >= thr_x10) {
        if (m->over_frames < 255) m->over_frames++;
        m->under_frames = 0;
    } else if (db <= release_x10) {
        if (m->under_frames < 255) m->under_frames++;
        m->over_frames = 0;
    } else {
        // 滞回带内:两个计数清零,保持当前告警状态(防边界抖动)。
        m->over_frames  = 0;
        m->under_frames = 0;
    }

    bool was = m->alarm_active;
    if (!m->alarm_active &&
        m->over_frames >= SOUND_METER_ALARM_ENTER_FRAMES) {
        m->alarm_active = true;
        m->alarm_count++;
    } else if (m->alarm_active &&
               m->under_frames >= SOUND_METER_ALARM_EXIT_FRAMES) {
        m->alarm_active = false;
    }
    return m->alarm_active != was;
}

void sound_meter_model_reset_session(sound_meter_model_t *m) {
    m->last_db_x10 = 0;
    m->peak_db_x10 = 0;
    m->db_sum_x10  = 0;
    m->frames      = 0;
    m->alarm_count = 0;
    m->over_frames = 0;
    m->under_frames = 0;
}

int32_t sound_meter_mean_db_x10(const sound_meter_model_t *m) {
    if (m->frames == 0) return 0;
    return (int32_t)(m->db_sum_x10 / (int64_t)m->frames);
}

uint32_t sound_meter_session_seconds(uint32_t frames) {
    return (uint32_t)(((uint64_t)frames * SOUND_METER_FRAME_MS) / 1000);
}

sound_meter_zone_t sound_meter_zone_of(int32_t db_x10) {
    if (db_x10 < 500) return SOUND_METER_ZONE_QUIET;
    if (db_x10 < 650) return SOUND_METER_ZONE_NORMAL;
    if (db_x10 < 750) return SOUND_METER_ZONE_MODERATE;
    if (db_x10 < 850) return SOUND_METER_ZONE_LOUD;
    return SOUND_METER_ZONE_EXTREME;
}

void sound_meter_wake_init(sound_meter_wake_t *w) {
    w->ema_db_x10 = 0;
    w->ready       = false;
    w->latched     = false;
    w->over_frames = 0;
}

bool sound_meter_wake_frame(sound_meter_wake_t *w, int32_t db_x10) {
    if (!w->ready) {                     // 首帧只建立 EMA 基线
        w->ema_db_x10 = db_x10;
        w->ready = true;
        return false;
    }
    int32_t delta = db_x10 - w->ema_db_x10;
    if (delta < 0) delta = -delta;
    // 先按更新前的偏离判定(反映突变),再让 EMA 缓慢跟上(权重 1/32)。
    w->ema_db_x10 += (db_x10 - w->ema_db_x10) >> 5;

    int32_t jump_x10 = SOUND_METER_WAKE_JUMP_DB * 10;
    if (w->latched) {                    // 已触发:等偏离回落后重新武装
        if (delta < jump_x10 / 2) w->latched = false;
        return false;
    }
    if (delta >= jump_x10) {
        if (w->over_frames < 255) w->over_frames++;
        if (w->over_frames >= SOUND_METER_WAKE_JUMP_FRAMES) {
            w->latched = true;           // 上升沿只报一次
            w->over_frames = 0;
            return true;
        }
    } else {
        w->over_frames = 0;              // 单帧毛刺:计数清零
    }
    return false;
}
