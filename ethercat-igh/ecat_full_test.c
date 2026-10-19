// ecat_full_test.c —— OPI5 原生 EtherCAT 全功能回归测试（意优 PHU20）
// DC(0x0300 SYNC0 500us) + 显式 OP + 主从时钟漂移监视 + 周期抖动 + WKC 统计。
// 安全：controlword 恒 0x0000，驱动器永不上电（Switch-on disabled）。
// 用法: ecat_full_test <seconds>
#include <ecrt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <inttypes.h>

#define SLAVE_POS 0
#define VID 0x00001097u
#define PID 0x00010002u
#define CYCLE_NS 500000

static volatile sig_atomic_t stop_flag = 0;
static void onint(int sig){ (void)sig; stop_flag = 1; }

static long tsdiff(struct timespec *a, struct timespec *b)
{
    return (a->tv_sec - b->tv_sec) * 1000000000L + (a->tv_nsec - b->tv_nsec);
}

int main(int argc, char **argv)
{
    int secs = (argc > 1) ? atoi(argv[1]) : 30;

    ec_master_t *master = ecrt_request_master(0);
    if (!master) { fprintf(stderr, "request_master failed\n"); return 1; }

    ec_domain_t *domain = ecrt_master_create_domain(master);
    if (!domain) return 1;

    ec_slave_config_t *sc = ecrt_master_slave_config(master, 0, SLAVE_POS, VID, PID);
    if (!sc) return 1;

    if (ecrt_slave_config_pdos(sc, EC_END, NULL)) {
        fprintf(stderr, "SII pdo config failed\n"); return 1;
    }

    /* DC：意优 ESI DC 同步模式 AssignActivate=0x0300，SYNC0=500us。
     * 必须同时写 SM 周期时间对象，否则驱动器校验不一致拒绝 OP。 */
    ecrt_slave_config_dc(sc, 0x0300, CYCLE_NS, 0, 0, 0);
    /* ETG 流程：DC 模式下应用必须在 PreOP 写 0x1c32/0x1c33:01 = 2（DC 同步）。
     * ESI 默认值 0x0001 是 SM 同步；本设备 0x1c32:02 周期只读（取自 ESC SYNC0）。 */
    if (ecrt_slave_config_sdo16(sc, 0x1c32, 1, 2)) {
        fprintf(stderr, "sdo 0x1c32:01=2 failed\n"); return 1;
    }
    if (ecrt_slave_config_sdo16(sc, 0x1c33, 1, 2)) {
        fprintf(stderr, "sdo 0x1c33:01=2 failed\n"); return 1;
    }

    int off_cw  = ecrt_slave_config_reg_pdo_entry(sc, 0x6040, 0, domain, NULL);
    int off_sw  = ecrt_slave_config_reg_pdo_entry(sc, 0x6041, 0, domain, NULL);
    int off_pos = ecrt_slave_config_reg_pdo_entry(sc, 0x6064, 0, domain, NULL);
    int off_vel = ecrt_slave_config_reg_pdo_entry(sc, 0x606c, 0, domain, NULL);
    int off_trq = ecrt_slave_config_reg_pdo_entry(sc, 0x6077, 0, domain, NULL);
    if (off_cw < 0 || off_sw < 0 || off_pos < 0 || off_vel < 0 || off_trq < 0) {
        fprintf(stderr, "reg_pdo_entry failed\n"); return 1;
    }
    printf("pdo offsets: cw=%d sw=%d pos=%d vel=%d trq=%d\n",
           off_cw, off_sw, off_pos, off_vel, off_trq);

    /* 运行期邮箱 SDO 请求（xbot CoeNode 同款：open 前创建，运行期提交） */
    ec_sdo_request_t *req_torque = ecrt_slave_config_create_sdo_request(sc, 0x6071, 0, 2);
    ec_sdo_request_t *req_name   = ecrt_slave_config_create_sdo_request(sc, 0x1008, 0, 64);
    if (!req_torque || !req_name) { fprintf(stderr, "create sdo_request failed\n"); return 1; }

    /* CompleteAccess 下载路径（--ca 显式开启）：写只读 0x1018 验证机制与
     * 错误处理；注意驱动器对这种语义错误的容忍不稳定（一次卡 PREOP），
     * 实际只用 CA 写 RW 对象。 */
    if (argc > 2 && strcmp(argv[2], "--ca") == 0) {
        uint8_t ca_buf[16] = {0};
        int car = ecrt_slave_config_complete_sdo(sc, 0x1018, ca_buf, sizeof(ca_buf));
        printf("CA download 0x1018: ret=%d\n", car);
    }

    /* 显式请求 OP（应用路径权威状态请求，与 xbot_io_ecat 一致） */
    ec_slave_config_state_t css;
    memset(&css, 0, sizeof(css));
    css.al_state = EC_AL_STATE_OP;
    ecrt_slave_config_state(sc, &css);

    if (ecrt_master_activate(master)) { fprintf(stderr, "activate failed\n"); return 1; }
    uint8_t *pd = ecrt_domain_data(domain);

    /* DC 应用时间基准：主站时钟为参考，必须每周期开环推进并喂给主站，
     * 否则 sync_reference_clock 写给从站的是冻结时间，DC 锁不上（0x2D）。 */
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    uint64_t app_time = (uint64_t)rt.tv_sec * 1000000000ULL + rt.tv_nsec;

    signal(SIGINT, onint);
    struct timespec next, now;
    clock_gettime(CLOCK_MONOTONIC, &next);
    int cycles = secs * 1000000000L / CYCLE_NS;
    long total = 0, bad = 0;
    int last_wkc = -1, wkc_changes = 0;
    long jmax = 0, jsum = 0, jn = 0, misses = 0;
    uint32_t drift_max = 0; uint64_t drift_sum = 0; long drift_n = 0;
    uint16_t sw_first = 0xffff, sw_last = 0;
    int32_t pos_last = 0;
    ec_domain_state_t ds;
    ec_master_state_t ms;
    unsigned int last_wkc_live = 0;
    long wmax = 0, wsum = 0, wn = 0;
    int op_seen = 0; long op_cycle = -1;

    while (total < cycles && !stop_flag) {
        next.tv_sec += (next.tv_nsec + CYCLE_NS) / 1000000000;
        next.tv_nsec = (next.tv_nsec + CYCLE_NS) % 1000000000;
        /* 尾部自旋：提前 3µs 睡醒，自旋到点——消除最后的唤醒抖动 */
        struct timespec wake = next;
        if (wake.tv_nsec >= 3000) wake.tv_nsec -= 3000;
        else { wake.tv_sec -= 1; wake.tv_nsec += 1000000000 - 3000; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, NULL);
        do { clock_gettime(CLOCK_MONOTONIC, &now); } while (tsdiff(&now, &next) < 0);
        long over = tsdiff(&now, &next);
        (void)over;
        if (over > 0) {
            if (over > jmax) jmax = over;
            jsum += over; jn++;
            if (over > CYCLE_NS / 2) misses++;
        }

        ecrt_master_receive(master);
        ecrt_domain_process(domain);
        if (total % 100 == 0) {
            ecrt_domain_state(domain, &ds);
            ecrt_master_state(master, &ms);
            last_wkc_live = ds.working_counter;
        } else {
            ds.working_counter = last_wkc_live;
        }
        uint32_t drift = ecrt_master_sync_monitor_process(master);
        if (last_wkc_live == 3 && drift > 0 && drift != 0xFFFFFFFFu) {  /* 只统计 OP 运行期 */
            if (drift > drift_max) drift_max = drift;
            drift_sum += drift; drift_n++;
        }
        /* 消费完上一帧的结果后立刻重挂，使数据报随本帧发出；
         * 若在 send 之后 queue，会把 SENT 态数据报再次入队（主站判定
         * UNMATCHED），漂移测量永远拿不到值。 */
        ecrt_master_sync_monitor_queue(master);

        total++;
        if (ds.working_counter != last_wkc) { wkc_changes++; last_wkc = ds.working_counter; }
        if (ds.working_counter == 0) bad++;
        if (!op_seen && (ms.al_states & 0x08)) { op_seen = 1; op_cycle = total; }

        sw_last = EC_READ_U16(pd + off_sw);
        pos_last = EC_READ_S32(pd + off_pos);
        static int sdo_phase = 0; static long sdo_start = 0;
        if (total == 3000) {   /* OP 稳定后：运行期邮箱写 0x6071=0（目标转矩 0，安全） */
            EC_WRITE_U16(ecrt_sdo_request_data(req_torque), 0);
            ecrt_sdo_request_write(req_torque);
            sdo_phase = 1; sdo_start = total;
        }
        if (sdo_phase == 1) {
            ec_request_state_t st = ecrt_sdo_request_state(req_torque);
            if (st == EC_REQUEST_SUCCESS) {
                printf("[SDO] 运行期写 0x6071=0 OK（%ld 周期完成）\n", total - sdo_start);
                sdo_phase = 2;
            } else if (st == EC_REQUEST_ERROR) {
                printf("[SDO] 运行期写 0x6071 失败（abort 码见 dmesg）\n");
                sdo_phase = 2;
            }
        }
        if (total == 6000) {   /* 运行期邮箱读 0x1008 设备名字符串 */
            ecrt_sdo_request_read(req_name);
            sdo_phase = 3; sdo_start = total;
        }
        if (sdo_phase == 3) {
            ec_request_state_t st = ecrt_sdo_request_state(req_name);
            if (st == EC_REQUEST_SUCCESS) {
                printf("[SDO] 运行期读 0x1008 = \"%s\"（%ld 周期）\n",
                       (char *)ecrt_sdo_request_data(req_name), total - sdo_start);
                sdo_phase = 4;
            } else if (st == EC_REQUEST_ERROR) {
                printf("[SDO] 运行期读 0x1008 失败（abort 码见 dmesg）\n");
                sdo_phase = 4;
            }
        }
        if (total == 1 || total == 100 || (total % 8000) == 0 || total == cycles) {
            printf("[%6ld] wkc=%u sw=0x%04x pos=%" PRId32 " al=0x%02x drift=%u ns\n",
                   total, ds.working_counter, sw_last, pos_last, ms.al_states, drift);
        }

        EC_WRITE_U16(pd + off_cw, 0x0000);    /* 永不上电 */
        app_time += CYCLE_NS;
        ecrt_master_application_time(master, app_time);
        ecrt_master_sync_reference_clock_to(master, app_time);  /* xbot IghBackend 同款 */
        ecrt_domain_queue(domain);
        ecrt_master_sync_slave_clocks(master);      /* 补偿从站时钟漂移 */
        ecrt_master_send(master);
        clock_gettime(CLOCK_MONOTONIC, &now);
        long work = tsdiff(&now, &next);
        if (work > wmax) wmax = work;
        wsum += work; wn++;
    }

    printf("\n== 汇总 ==\n");
    printf("周期: %ld, wkc=0 周期数: %ld, wkc 跳变: %d\n", total, bad, wkc_changes);
    printf("OP 进入周期: %ld (al 状态含 0x08: %s)\n", op_cycle, op_seen ? "yes" : "NO");
    printf("唤醒超期 ns: max=%ld mean=%ld n=%ld, 半周期以上错失: %ld\n",
           jmax, jn ? jsum / jn : 0, jn, misses);
    printf("单周期工作 ns: max=%ld mean=%ld n=%ld\n", wmax, wn ? wsum / wn : 0, wn);
    printf("主从时钟漂移 ns: max=%u mean=%u n=%ld\n",
           drift_max, drift_n ? (uint32_t)(drift_sum / drift_n) : 0, drift_n);
    printf("statusword last=0x%04x pos last=%" PRId32 "\n", sw_last, pos_last);

    /* 干净退出：回 SAFEOP/PREOP，让主站回 idle */
    memset(&css, 0, sizeof(css));
    css.al_state = EC_AL_STATE_PREOP;
    ecrt_slave_config_state(sc, &css);
    for (int i = 0; i < 20; i++) {
        ecrt_master_receive(master);
        ecrt_domain_process(domain);
        EC_WRITE_U16(pd + off_cw, 0x0000);
        ecrt_domain_queue(domain);
        ecrt_master_send(master);
        usleep(10000);
    }
    ecrt_release_master(master);
    return 0;
}
