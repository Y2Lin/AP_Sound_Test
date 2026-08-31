// tests/test_sound_meter_model.c -- 音量检测纯逻辑的主机侧测试(gcc 编译即跑,无需硬件)。
// 期望值由双精度参考公式 200*log10(rms/32768) 换算,容差 ±2(0.2 dB),
// 覆盖定点查表、线性插值与舍入的全部误差来源。
#include <stdio.h>
#include <stdbool.h>
#include "sound_meter_model.h"

// 断言失败时打印位置与实际值,返回 1 由 main 汇总退出码。
#define EXPECT_NEAR(actual, expected, tol)                                   \
    do {                                                                     \
        long long a_ = (long long)(actual), e_ = (long long)(expected);      \
        long long d_ = a_ - e_;                                              \
        if (d_ < 0) d_ = -d_;                                                \
        if (d_ > (long long)(tol)) {                                         \
            fprintf(stderr, "FAIL %s:%d %s = %lld (want %lld +/- %d)\n",     \
                    __FILE__, __LINE__, #actual, a_, e_, (int)(tol));        \
            return 1;                                                        \
        }                                                                    \
    } while (0)

#define EXPECT_EQ(actual, expected)                                          \
    do {                                                                     \
        long long a_ = (long long)(actual), e_ = (long long)(expected);      \
        if (a_ != e_) {                                                      \
            fprintf(stderr, "FAIL %s:%d %s = %lld (want %lld)\n",            \
                    __FILE__, __LINE__, #actual, a_, e_);                    \
            return 1;                                                        \
        }                                                                    \
    } while (0)

// 测试用的代表帧:50/60/63/70 dB 对应的 RMS(参考公式反推)。
#define RMS_50DB 207u   // display 500
#define RMS_60DB 654u   // display 600
#define RMS_63DB 924u   // display 630(阈值 65 的滞回带内)
#define RMS_70DB 2072u  // display 700

// 连续喂 n 帧,返回最后一帧的告警翻转边沿。
static bool feed(sound_meter_model_t *m, uint32_t rms, int n) {
    bool edge = false;
    for (int i = 0; i < n; i++) edge = sound_meter_model_frame(m, rms);
    return edge;
}

static int test_isqrt(void) {
    EXPECT_EQ(sound_meter_isqrt_u64(0), 0);
    EXPECT_EQ(sound_meter_isqrt_u64(1), 1);
    EXPECT_EQ(sound_meter_isqrt_u64(2), 1);
    EXPECT_EQ(sound_meter_isqrt_u64(3), 1);
    EXPECT_EQ(sound_meter_isqrt_u64(4), 2);
    EXPECT_EQ(sound_meter_isqrt_u64(8), 2);
    EXPECT_EQ(sound_meter_isqrt_u64(9), 3);
    EXPECT_EQ(sound_meter_isqrt_u64(15), 3);
    EXPECT_EQ(sound_meter_isqrt_u64(16), 4);
    EXPECT_EQ(sound_meter_isqrt_u64(24), 4);
    EXPECT_EQ(sound_meter_isqrt_u64(25), 5);
    EXPECT_EQ(sound_meter_isqrt_u64(65535), 255);
    EXPECT_EQ(sound_meter_isqrt_u64(65536), 256);
    EXPECT_EQ(sound_meter_isqrt_u64(999999999999u), 999999);
    EXPECT_EQ(sound_meter_isqrt_u64(4294836225u), 65535);   // 65535^2
    EXPECT_EQ(sound_meter_isqrt_u64(4294967295u), 65535);   // 2^32-1
    EXPECT_EQ(sound_meter_isqrt_u64(18446744065119617025u), 4294967295u); // (2^32-1)^2
    return 0;
}

static int test_dbfs(void) {
    EXPECT_NEAR(sound_meter_dbfs_x10(16384), -60, 2);    // -6.02 dB
    EXPECT_NEAR(sound_meter_dbfs_x10(1000), -303, 2);    // -30.31 dB
    EXPECT_NEAR(sound_meter_dbfs_x10(256), -421, 2);     // -42.14 dB
    EXPECT_NEAR(sound_meter_dbfs_x10(32767), 0, 2);      // -0.00 dB
    EXPECT_NEAR(sound_meter_dbfs_x10(1), -903, 2);       // -90.31 dB
    EXPECT_EQ(sound_meter_dbfs_x10(0), -903);            // 静音取下限

    EXPECT_NEAR(sound_meter_display_db_x10(32767), 940, 2);
    EXPECT_NEAR(sound_meter_display_db_x10(207), 500, 2);
    EXPECT_NEAR(sound_meter_display_db_x10(654), 600, 2);
    EXPECT_NEAR(sound_meter_display_db_x10(924), 630, 2);
    EXPECT_NEAR(sound_meter_display_db_x10(2072), 700, 2);
    EXPECT_EQ(sound_meter_display_db_x10(0), 37);        // 940-903

    // 单调性:抽样遍历不下降。
    int32_t prev = sound_meter_dbfs_x10(1);
    for (uint32_t r = 998; r < 32768; r += 997) {
        int32_t cur = sound_meter_dbfs_x10(r);
        if (cur < prev) {
            fprintf(stderr, "FAIL %s:%d dbfs not monotonic at rms=%u\n",
                    __FILE__, __LINE__, (unsigned)r);
            return 1;
        }
        prev = cur;
    }
    if (sound_meter_dbfs_x10(32767) < prev) {   // 终点覆盖最大值
        fprintf(stderr, "FAIL %s:%d dbfs(32767) below sampled tail\n",
                __FILE__, __LINE__);
        return 1;
    }
    return 0;
}

static int test_bar_pct(void) {
    EXPECT_EQ(sound_meter_bar_pct(0), 0);
    EXPECT_EQ(sound_meter_bar_pct(250), 0);
    EXPECT_EQ(sound_meter_bar_pct(300), 0);
    EXPECT_EQ(sound_meter_bar_pct(301), 0);
    EXPECT_EQ(sound_meter_bar_pct(599), 49);
    EXPECT_EQ(sound_meter_bar_pct(600), 50);
    EXPECT_EQ(sound_meter_bar_pct(601), 50);
    EXPECT_EQ(sound_meter_bar_pct(899), 99);
    EXPECT_EQ(sound_meter_bar_pct(900), 100);
    EXPECT_EQ(sound_meter_bar_pct(950), 100);
    EXPECT_EQ(sound_meter_bar_pct(1200), 100);
    for (int32_t db = 1; db < 1200; db += 37) {
        if (sound_meter_bar_pct(db) > sound_meter_bar_pct(db + 37)) {
            fprintf(stderr, "FAIL %s:%d bar_pct not monotonic at %d\n",
                    __FILE__, __LINE__, (int)db);
            return 1;
        }
    }
    return 0;
}

static int test_threshold(void) {
    EXPECT_EQ(sound_meter_threshold_step(65, 1), 66);
    EXPECT_EQ(sound_meter_threshold_step(65, -1), 64);
    EXPECT_EQ(sound_meter_threshold_step(65, 5), 70);
    EXPECT_EQ(sound_meter_threshold_step(65, -5), 60);
    EXPECT_EQ(sound_meter_threshold_step(99, 1), 100);
    EXPECT_EQ(sound_meter_threshold_step(100, 1), 100);   // 上界钳位
    EXPECT_EQ(sound_meter_threshold_step(100, -1), 99);
    EXPECT_EQ(sound_meter_threshold_step(40, -1), 40);    // 下界钳位
    EXPECT_EQ(sound_meter_threshold_step(40, -5), 40);
    EXPECT_EQ(sound_meter_threshold_step(41, -5), 40);
    EXPECT_EQ(sound_meter_threshold_step(98, 5), 100);

    EXPECT_EQ(sound_meter_threshold_load(65), 65);
    EXPECT_EQ(sound_meter_threshold_load(40), 40);
    EXPECT_EQ(sound_meter_threshold_load(100), 100);
    EXPECT_EQ(sound_meter_threshold_load(39), 65);        // 越界回退默认
    EXPECT_EQ(sound_meter_threshold_load(101), 65);
    EXPECT_EQ(sound_meter_threshold_load(0), 65);         // NVS 未写入值
    EXPECT_EQ(sound_meter_threshold_load(255), 65);
    EXPECT_EQ(sound_meter_threshold_load(-1), 65);
    return 0;
}

static int test_alarm_fsm(void) {
    sound_meter_model_t m;
    sound_meter_model_init(&m, 65);

    // 安静若干帧:不触发。
    EXPECT_EQ(feed(&m, RMS_50DB, 5), false);
    EXPECT_EQ(m.alarm_active, false);

    // 单帧尖峰不触发(去抖:需连续 3 帧)。
    EXPECT_EQ(sound_meter_model_frame(&m, RMS_70DB), false);
    EXPECT_EQ(m.alarm_active, false);
    EXPECT_EQ(sound_meter_model_frame(&m, RMS_50DB), false);
    EXPECT_EQ(m.alarm_active, false);

    // 连续 3 帧超阈值:第 3 帧触发并计数。
    EXPECT_EQ(sound_meter_model_frame(&m, RMS_70DB), false);
    EXPECT_EQ(sound_meter_model_frame(&m, RMS_70DB), false);
    EXPECT_EQ(sound_meter_model_frame(&m, RMS_70DB), true);
    EXPECT_EQ(m.alarm_active, true);
    EXPECT_EQ(m.alarm_count, 1);

    // 滞回带内(阈值-2 dB)持续:保持告警。
    EXPECT_EQ(feed(&m, RMS_63DB, 6), false);
    EXPECT_EQ(m.alarm_active, true);
    EXPECT_EQ(m.alarm_count, 1);

    // 低于解除线(阈值-4 dB)需连续 5 帧:第 5 帧解除。
    EXPECT_EQ(feed(&m, RMS_60DB, 4), false);
    EXPECT_EQ(m.alarm_active, true);
    EXPECT_EQ(sound_meter_model_frame(&m, RMS_60DB), true);
    EXPECT_EQ(m.alarm_active, false);
    EXPECT_EQ(m.alarm_count, 1);

    // 再次触发:计数递增。
    EXPECT_EQ(feed(&m, RMS_70DB, 3), true);
    EXPECT_EQ(m.alarm_active, true);
    EXPECT_EQ(m.alarm_count, 2);
    return 0;
}

static int test_stats(void) {
    sound_meter_model_t m;
    sound_meter_model_init(&m, 65);

    // 50/63/70 dB 各一帧:峰值 70,均值 61,无告警。
    sound_meter_model_frame(&m, RMS_50DB);
    sound_meter_model_frame(&m, RMS_63DB);
    sound_meter_model_frame(&m, RMS_70DB);
    EXPECT_EQ(m.frames, 3);
    EXPECT_NEAR(m.peak_db_x10, 700, 2);
    EXPECT_NEAR(sound_meter_mean_db_x10(&m), 610, 2);
    EXPECT_EQ(m.alarm_count, 0);

    // 再补一帧 50 dB:均值截断除法(2330/4=582.5)。
    sound_meter_model_frame(&m, RMS_50DB);
    EXPECT_NEAR(sound_meter_mean_db_x10(&m), 582, 1);

    // 时长换算:16ms/帧,向下取整。
    EXPECT_EQ(sound_meter_session_seconds(0), 0);
    EXPECT_EQ(sound_meter_session_seconds(1), 0);
    EXPECT_EQ(sound_meter_session_seconds(62), 0);
    EXPECT_EQ(sound_meter_session_seconds(63), 1);
    EXPECT_EQ(sound_meter_session_seconds(625), 10);
    EXPECT_EQ(sound_meter_session_seconds(3750), 60);
    EXPECT_EQ(sound_meter_session_seconds(60000), 960);

    // 空模型:峰值/均值回落为 0。
    sound_meter_model_t empty;
    sound_meter_model_init(&empty, 65);
    EXPECT_EQ(empty.peak_db_x10, 0);
    EXPECT_EQ(sound_meter_mean_db_x10(&empty), 0);

    // 重置会话:统计清零,阈值与告警状态保留。
    sound_meter_model_reset_session(&m);
    EXPECT_EQ(m.frames, 0);
    EXPECT_EQ(m.peak_db_x10, 0);
    EXPECT_EQ(m.db_sum_x10, 0);
    EXPECT_EQ(m.alarm_count, 0);
    EXPECT_EQ(m.threshold_db, 65);
    EXPECT_EQ(m.alarm_active, false);
    return 0;
}

static int test_smooth(void) {
    sound_meter_model_t m;
    sound_meter_model_init(&m, 65);
    EXPECT_EQ(sound_meter_smooth_db_x10(&m), 0);      // 未喂帧为 0

    // 首帧直通:立即建立基线,无起步爬坡。
    (void)sound_meter_model_frame(&m, RMS_50DB);
    EXPECT_NEAR(sound_meter_smooth_db_x10(&m), 500, 2);

    // 恒定电平:长期喂入仍收敛在原值(±0.2dB),不漂移。
    EXPECT_EQ(feed(&m, RMS_50DB, 100), false);
    EXPECT_NEAR(sound_meter_smooth_db_x10(&m), 500, 2);

    // 突升 20dB:快通道第 1 帧走约 1/8(+2.5dB),不是瞬时跳变;
    // 8 帧(~130ms)走完约 2/3;40 帧后收敛。
    (void)sound_meter_model_frame(&m, RMS_70DB);
    int32_t s1 = sound_meter_smooth_db_x10(&m);
    if (s1 < 515 || s1 > 545) {
        fprintf(stderr, "FAIL %s:%d smooth attack frame1 = %lld (want ~525)\n",
                __FILE__, __LINE__, (long long)s1);
        return 1;
    }
    (void)feed(&m, RMS_70DB, 7);                      // 累计 8 帧
    int32_t s8 = sound_meter_smooth_db_x10(&m);
    if (s8 < 610 || s8 > 690) {   // (7/8)^8≈0.34 -> 走完约 66%
        fprintf(stderr, "FAIL %s:%d smooth attack frame8 = %lld (want ~631)\n",
                __FILE__, __LINE__, (long long)s8);
        return 1;
    }
    (void)feed(&m, RMS_70DB, 40);
    EXPECT_NEAR(sound_meter_smooth_db_x10(&m), 700, 2);

    // 原始读数与峰值统计不经过平滑(峰值抓真瞬态)。
    EXPECT_NEAR(m.last_db_x10, 700, 2);
    EXPECT_NEAR(m.peak_db_x10, 700, 2);

    // 突降 20dB:慢通道 8 帧后只走约 1/4(~0.5s 时间常数),读数缓落。
    (void)feed(&m, RMS_50DB, 8);
    int32_t d8 = sound_meter_smooth_db_x10(&m);
    if (d8 < 630 || d8 > 695) {   // (31/32)^8≈0.78 -> 还剩约 78%
        fprintf(stderr, "FAIL %s:%d smooth release frame8 = %lld (want ~655)\n",
                __FILE__, __LINE__, (long long)d8);
        return 1;
    }
    (void)feed(&m, RMS_50DB, 200);
    EXPECT_NEAR(sound_meter_smooth_db_x10(&m), 500, 2);

    // 清零会话统计:平滑器保留(环境响度没变,上屏读数不跳变)。
    sound_meter_model_reset_session(&m);
    EXPECT_NEAR(sound_meter_smooth_db_x10(&m), 500, 2);
    return 0;
}

static int test_wake(void) {
    sound_meter_wake_t w;
    sound_meter_wake_init(&w);
    EXPECT_EQ(w.ready, false);
    EXPECT_EQ(w.latched, false);

    // 首帧只建基线,不触发。
    EXPECT_EQ(sound_meter_wake_frame(&w, 500), false);
    EXPECT_EQ(w.ready, true);
    for (int i = 0; i < 10; i++)
        EXPECT_EQ(sound_meter_wake_frame(&w, 500), false);

    // 单帧尖峰(毛刺):连续帧数不足,不触发。
    EXPECT_EQ(sound_meter_wake_frame(&w, 800), false);
    EXPECT_EQ(sound_meter_wake_frame(&w, 500), false);
    EXPECT_EQ(sound_meter_wake_frame(&w, 500), false);

    // 突升 30dB 且连续 2 帧 -> 第 2 帧触发(上升沿只报一次)。
    EXPECT_EQ(sound_meter_wake_frame(&w, 800), false);
    EXPECT_EQ(sound_meter_wake_frame(&w, 800), true);
    for (int i = 0; i < 5; i++)
        EXPECT_EQ(sound_meter_wake_frame(&w, 800), false);   // 锁存期不重复

    // EMA 追上后锁存释放(300*(31/32)^n < 100 约需 35 帧,喂 40 帧)。
    for (int i = 0; i < 40; i++) (void)sound_meter_wake_frame(&w, 800);
    EXPECT_EQ(w.latched, false);

    // 突降(从响变静)同样算剧变 -> 触发。
    EXPECT_EQ(sound_meter_wake_frame(&w, 500), false);
    EXPECT_EQ(sound_meter_wake_frame(&w, 500), true);

    // 缓变(每帧 +0.5dB,持续 60 帧 = +30dB)不触发:
    // EMA 稳态滞后 = 坡度*31 = 15.5dB < 20dB 阈值。
    sound_meter_wake_init(&w);
    (void)sound_meter_wake_frame(&w, 500);
    for (int i = 1; i <= 60; i++) {
        if (sound_meter_wake_frame(&w, 500 + i * 5)) {
            fprintf(stderr, "FAIL %s:%d gradual ramp triggered at frame %d\n",
                    __FILE__, __LINE__, i);
            return 1;
        }
    }
    return 0;
}

static int test_zones(void) {
    EXPECT_EQ(sound_meter_zone_of(0), SOUND_METER_ZONE_QUIET);
    EXPECT_EQ(sound_meter_zone_of(370), SOUND_METER_ZONE_QUIET);   // 静音下限
    EXPECT_EQ(sound_meter_zone_of(499), SOUND_METER_ZONE_QUIET);
    EXPECT_EQ(sound_meter_zone_of(500), SOUND_METER_ZONE_NORMAL);  // 边界含
    EXPECT_EQ(sound_meter_zone_of(649), SOUND_METER_ZONE_NORMAL);
    EXPECT_EQ(sound_meter_zone_of(650), SOUND_METER_ZONE_MODERATE);
    EXPECT_EQ(sound_meter_zone_of(749), SOUND_METER_ZONE_MODERATE);
    EXPECT_EQ(sound_meter_zone_of(750), SOUND_METER_ZONE_LOUD);
    EXPECT_EQ(sound_meter_zone_of(849), SOUND_METER_ZONE_LOUD);
    EXPECT_EQ(sound_meter_zone_of(850), SOUND_METER_ZONE_EXTREME);
    EXPECT_EQ(sound_meter_zone_of(1200), SOUND_METER_ZONE_EXTREME); // 钳位上限
    return 0;
}

int main(void) {
    if (test_isqrt())     return 1;
    if (test_dbfs())      return 1;
    if (test_bar_pct())   return 1;
    if (test_threshold()) return 1;
    if (test_alarm_fsm()) return 1;
    if (test_stats())     return 1;
    if (test_smooth())    return 1;
    if (test_wake())      return 1;
    if (test_zones())     return 1;
    printf("sound_meter_model tests: PASS\n");
    return 0;
}
