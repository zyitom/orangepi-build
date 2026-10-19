// ecat_test.c —— OPI5 原生 EtherCAT 应用级验证（意优 PHU20）
// SII PDO 配置 + 500us 周期域交换 + OP 状态机；controlword 恒 0x0000，
// 驱动器永远停在 Switch-on disabled（不上电、零转矩），只验总线。
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

static volatile sig_atomic_t stop_flag = 0;
static void onint(int sig){ (void)sig; stop_flag = 1; }

int main(int argc, char **argv)
{
    int secs = (argc > 1) ? atoi(argv[1]) : 10;

    ec_master_t *master = ecrt_request_master(0);
    if (!master) { fprintf(stderr, "request_master failed\n"); return 1; }

    ec_domain_t *domain = ecrt_master_create_domain(master);
    if (!domain) return 1;

    ec_slave_config_t *sc = ecrt_master_slave_config(master, 0, SLAVE_POS, VID, PID);
    if (!sc) return 1;

    if (ecrt_slave_config_pdos(sc, EC_END, NULL)) {
        fprintf(stderr, "SII pdo config failed\n"); return 1;
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

    ec_slave_config_state_t css = { .al_state = EC_AL_STATE_OP };
    ecrt_slave_config_state(sc, &css);

    if (ecrt_master_activate(master)) { fprintf(stderr, "activate failed\n"); return 1; }
    uint8_t *pd = ecrt_domain_data(domain);

    signal(SIGINT, onint);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    int cycles = secs * 2000;                 /* 500us 周期 */
    long total = 0, bad = 0;
    int last_wkc = -1, wkc_changes = 0;
    uint16_t sw_first = 0xffff, sw_last = 0;
    int32_t pos_first = 0, pos_last = 0;
    ec_domain_state_t ds;
    ec_master_state_t ms;

    for (int i = 0; i < cycles && !stop_flag; i++) {
        next.tv_nsec += 500000;
        if (next.tv_nsec >= 1000000000) { next.tv_sec += 1; next.tv_nsec -= 1000000000; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        ecrt_master_receive(master);
        ecrt_domain_process(domain);
        ecrt_domain_state(domain, &ds);
        ecrt_master_state(master, &ms);

        total++;
        if (ds.working_counter != last_wkc) { wkc_changes++; last_wkc = ds.working_counter; }
        if (ds.working_counter == 0) bad++;

        if (i == 100) { sw_first = EC_READ_U16(pd + off_sw); pos_first = EC_READ_S32(pd + off_pos); }
        sw_last = EC_READ_U16(pd + off_sw);
        pos_last = EC_READ_S32(pd + off_pos);
        if (i == 100 || i == cycles - 1 || (i % 4000) == 0) {
            printf("[%5d] wkc=%u sw=0x%04x pos=%" PRId32 " vel=%" PRId32 " trq=%u "
                   "slaves=%u al=0x%02x link=%d\n", i, ds.working_counter,
                   sw_last, pos_last, EC_READ_S32(pd + off_vel),
                   EC_READ_U16(pd + off_trq), ms.slaves_responding, ms.al_states, ms.link_up);
        }

        EC_WRITE_U16(pd + off_cw, 0x0000);    /* 永不上电 */
        ecrt_domain_queue(domain);
        ecrt_master_send(master);
    }

    printf("\n== 汇总 ==\n周期: %ld, wkc=0 周期数: %ld, wkc 跳变次数: %d\n",
           total, bad, wkc_changes);
    printf("statusword: first=0x%04x last=0x%04x  position: first=%" PRId32 " last=%" PRId32 "\n",
           sw_first, sw_last, pos_first, pos_last);
    ecrt_release_master(master);
    return 0;
}
