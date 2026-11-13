// LicheeRV Nano / SG2002: measure real CVITEK VPSS frame delivery at 1920x1080.
// This is a capture benchmark: no OpenCV, network server, or video encoder.
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sample_comm.h"

#define OUT_W 1920U
#define OUT_H 1080U
#define VPSS_GRP_ID 0
#define VPSS_CHN_ID 0
#define OUTPUT_BLOCKS 5U

static volatile sig_atomic_t stop_requested = 0;
static void on_signal(int signo) { (void)signo; stop_requested = 1; }

static double monotonic_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1000000000.0;
}

static int sdk_check(const char *what, CVI_S32 code)
{
    if (code == CVI_SUCCESS)
        return 0;
    fprintf(stderr, "ERROR: %s returned %#x\n", what, (unsigned int)code);
    return -1;
}

static int positive_int(const char *s, int *n)
{
    char *end = NULL;
    long val;
    errno = 0;
    val = strtol(s, &end, 10);
    if (errno || end == s || *end != '\0' || val < 1 || val > 1000000)
        return -1;
    *n = (int)val;
    return 0;
}

int main(int argc, char **argv)
{
    int target_frames = 300;
    int warmup_frames = 30;
    int i;
    int result = 1;
    int sys_ready = 0, vi_ready = 0, vpss_created = 0;
    int ch_enabled = 0, grp_started = 0, vi_bound = 0;
    VB_POOL output_pool = VB_INVALID_POOLID;
    SAMPLE_INI_CFG_S ini = {0};
    SAMPLE_VI_CONFIG_S vi_cfg;
    PIC_SIZE_E input_pic;
    SIZE_S sensor_size = {0};
    VB_CONFIG_S common_vb = {0};
    VB_POOL_CONFIG_S out_vb = {0};
    VPSS_GRP_ATTR_S grp_attr = {0};
    VPSS_CHN_ATTR_S ch_attr = {0};
    CVI_U32 source_blk, output_blk;
    CVI_S32 rc;
    int successful = 0, failed = 0, wrong = 0, duplicate = 0;
    int samples = 0, have_prev = 0;
    CVI_U64 prev_pts = 0;
    CVI_U32 prev_ref = 0;
    double first_t = 0.0, last_t = 0.0, fps = 0.0;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            if (positive_int(argv[++i], &target_frames)) goto usage;
        } else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            if (positive_int(argv[++i], &warmup_frames)) goto usage;
        } else {
usage:
            fprintf(stderr, "Usage: %s [--frames N] [--warmup N]\n", argv[0]);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    setvbuf(stdout, NULL, _IOLBF, 0);

    ini.enSource = VI_PIPE_FRAME_SOURCE_DEV;
    ini.devNum = 1;
    ini.enSnsType[0] = GCORE_GC4653_MIPI_4M_30FPS_10BIT;
    ini.enWDRMode[0] = WDR_MODE_NONE;
    ini.s32BusId[0] = 4;
    ini.s32SnsI2cAddr[0] = 0x1d;
    ini.MipiDev[0] = 0;

    if (access("/mnt/data/sensor_cfg.ini", R_OK) != 0) {
        fprintf(stderr, "ERROR: missing /mnt/data/sensor_cfg.ini\n");
        goto cleanup;
    }
    if (sdk_check("SAMPLE_COMM_VI_SetIniPath",
                  SAMPLE_COMM_VI_SetIniPath("/mnt/data/sensor_cfg.ini"))) goto cleanup;
    if (sdk_check("SAMPLE_COMM_VI_ParseIni", SAMPLE_COMM_VI_ParseIni(&ini))) goto cleanup;
    if (ini.devNum != 1 || ini.enSnsType[0] != GCORE_GC4653_MIPI_4M_30FPS_10BIT) {
        fprintf(stderr, "ERROR: expected exactly one GC4653 sensor in sensor_cfg.ini\n");
        goto cleanup;
    }
    if (sdk_check("SAMPLE_COMM_VI_IniToViCfg", SAMPLE_COMM_VI_IniToViCfg(&ini, &vi_cfg))) goto cleanup;
    if (sdk_check("SAMPLE_COMM_VI_GetSizeBySensor",
                  SAMPLE_COMM_VI_GetSizeBySensor(ini.enSnsType[0], &input_pic))) goto cleanup;
    if (sdk_check("SAMPLE_COMM_SYS_GetPicSize",
                  SAMPLE_COMM_SYS_GetPicSize(input_pic, &sensor_size))) goto cleanup;

    if (sensor_size.u32Width < OUT_W || sensor_size.u32Height < OUT_H) {
        fprintf(stderr, "ERROR: sensor %ux%u smaller than output %ux%u\n",
                sensor_size.u32Width, sensor_size.u32Height, OUT_W, OUT_H);
        goto cleanup;
    }
    printf("[CONFIG] sensor=%ux%u -> VPSS=%ux%u NV21, target=%d frames after %d warmup\n",
           sensor_size.u32Width, sensor_size.u32Height,
           OUT_W, OUT_H, target_frames, warmup_frames);

    /* Ensure a common pool for VI and a SEPARATE, explicitly attached VPSS pool.
       A free larger common pool is not used as an automatic fallback when a
       smaller selected pool is exhausted in the SG2002 driver. */
    source_blk = COMMON_GetPicBufferSize(sensor_size.u32Width, sensor_size.u32Height,
                  PIXEL_FORMAT_NV21, DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
    /* Leave sufficient room for internal 4:2:2 input/pipeline buffers too. */
    if (source_blk < sensor_size.u32Width * sensor_size.u32Height * 2U)
        source_blk = sensor_size.u32Width * sensor_size.u32Height * 2U;
    output_blk = COMMON_GetPicBufferSize(OUT_W, OUT_H,
                  PIXEL_FORMAT_NV21, DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
    if (!source_blk || !output_blk) {
        fprintf(stderr, "ERROR: COMMON_GetPicBufferSize returned zero\n");
        goto cleanup;
    }
    common_vb.u32MaxPoolCnt = 1;
    common_vb.astCommPool[0].u32BlkSize = source_blk;
    common_vb.astCommPool[0].u32BlkCnt = 3;
    common_vb.astCommPool[0].enRemapMode = VB_REMAP_MODE_CACHED;
    strncpy(common_vb.astCommPool[0].acName, "vi_gc4653", MAX_VB_POOL_NAME_LEN - 1);

    if (sdk_check("SAMPLE_COMM_SYS_Init", SAMPLE_COMM_SYS_Init(&common_vb))) goto cleanup;
    sys_ready = 1;

    out_vb.u32BlkSize = output_blk;
    out_vb.u32BlkCnt = OUTPUT_BLOCKS;
    out_vb.enRemapMode = VB_REMAP_MODE_CACHED;
    strncpy(out_vb.acName, "vpss_fhd30", MAX_VB_POOL_NAME_LEN - 1);
    output_pool = CVI_VB_CreatePool(&out_vb);
    if (output_pool == VB_INVALID_POOLID) {
        fprintf(stderr, "ERROR: CVI_VB_CreatePool(output) failed\n");
        goto cleanup;
    }
    printf("[VB] VI pool: %u bytes x 3; VPSS pool: id=%u %u bytes x %u\n",
           source_blk, output_pool, output_blk, OUTPUT_BLOCKS);

    if (sdk_check("CVI_VI_SetDevNum", CVI_VI_SetDevNum(1))) goto cleanup;
    if (sdk_check("SAMPLE_PLAT_VI_INIT", SAMPLE_PLAT_VI_INIT(&vi_cfg))) goto cleanup;
    vi_ready = 1;

    grp_attr.u32MaxW = sensor_size.u32Width;
    grp_attr.u32MaxH = sensor_size.u32Height;
    grp_attr.enPixelFormat = PIXEL_FORMAT_NV21;
    grp_attr.stFrameRate.s32SrcFrameRate = -1;
    grp_attr.stFrameRate.s32DstFrameRate = -1;
    grp_attr.u8VpssDev = 0;
    if (sdk_check("CVI_VPSS_CreateGrp", CVI_VPSS_CreateGrp(VPSS_GRP_ID, &grp_attr))) goto cleanup;
    vpss_created = 1;
    if (sdk_check("CVI_VPSS_ResetGrp", CVI_VPSS_ResetGrp(VPSS_GRP_ID))) goto cleanup;

    ch_attr.u32Width = OUT_W;
    ch_attr.u32Height = OUT_H;
    ch_attr.enVideoFormat = VIDEO_FORMAT_LINEAR;
    ch_attr.enPixelFormat = PIXEL_FORMAT_NV21;
    ch_attr.stFrameRate.s32SrcFrameRate = -1; /* no artificial throttling */
    ch_attr.stFrameRate.s32DstFrameRate = -1;
    ch_attr.u32Depth = 3;
    ch_attr.bMirror = CVI_FALSE;
    ch_attr.bFlip = CVI_FALSE;
    ch_attr.stAspectRatio.enMode = ASPECT_RATIO_NONE;
    ch_attr.stNormalize.bEnable = CVI_FALSE;
    if (sdk_check("CVI_VPSS_SetChnAttr", CVI_VPSS_SetChnAttr(VPSS_GRP_ID, VPSS_CHN_ID, &ch_attr))) goto cleanup;
    if (sdk_check("CVI_VPSS_AttachVbPool",
                  CVI_VPSS_AttachVbPool(VPSS_GRP_ID, VPSS_CHN_ID, output_pool))) goto cleanup;
    if (sdk_check("CVI_VPSS_EnableChn", CVI_VPSS_EnableChn(VPSS_GRP_ID, VPSS_CHN_ID))) goto cleanup;
    ch_enabled = 1;
    if (sdk_check("CVI_VPSS_StartGrp", CVI_VPSS_StartGrp(VPSS_GRP_ID))) goto cleanup;
    grp_started = 1;
    if (sdk_check("SAMPLE_COMM_VI_Bind_VPSS",
                  SAMPLE_COMM_VI_Bind_VPSS(0, 0, VPSS_GRP_ID))) goto cleanup;
    vi_bound = 1;

    while (!stop_requested && samples < warmup_frames + target_frames) {
        VIDEO_FRAME_INFO_S frame = {0};
        rc = CVI_VPSS_GetChnFrame(VPSS_GRP_ID, VPSS_CHN_ID, &frame, 1000);
        if (rc != CVI_SUCCESS) {
            failed++;
            if (failed <= 5 || failed % 30 == 0)
                fprintf(stderr, "[WARN] CVI_VPSS_GetChnFrame error %#x (count=%d)\n",
                        (unsigned int)rc, failed);
            if (failed >= 10) break;
            continue;
        }

        if (frame.stVFrame.u32Width != OUT_W || frame.stVFrame.u32Height != OUT_H ||
            frame.stVFrame.enPixelFormat != PIXEL_FORMAT_NV21) {
            wrong++;
            fprintf(stderr, "[ERROR] Unexpected frame: %ux%u format=%d\n",
                    frame.stVFrame.u32Width, frame.stVFrame.u32Height,
                    (int)frame.stVFrame.enPixelFormat);
        }
        if (samples == 0) {
            printf("[FRAME] %ux%u format=%d stride=%u/%u length=%u/%u pts=%" PRIu64 "\n",
                frame.stVFrame.u32Width, frame.stVFrame.u32Height,
                (int)frame.stVFrame.enPixelFormat,
                frame.stVFrame.u32Stride[0], frame.stVFrame.u32Stride[1],
                frame.stVFrame.u32Length[0], frame.stVFrame.u32Length[1],
                (uint64_t)frame.stVFrame.u64PTS);
        }
        if (samples >= warmup_frames) {
            const double t = monotonic_seconds();
            if (successful == 0) first_t = t;
            last_t = t;
            if (have_prev && frame.stVFrame.u64PTS == prev_pts &&
                frame.stVFrame.u32TimeRef == prev_ref) duplicate++;
            prev_pts = frame.stVFrame.u64PTS;
            prev_ref = frame.stVFrame.u32TimeRef;
            have_prev = 1;
            successful++;
        }
        samples++;
        if (sdk_check("CVI_VPSS_ReleaseChnFrame",
                      CVI_VPSS_ReleaseChnFrame(VPSS_GRP_ID, VPSS_CHN_ID, &frame))) {
            failed++;
            break;
        }
        if (successful > 0 && successful % 100 == 0)
            printf("[PROGRESS] frames=%d elapsed=%.3fs fps=%.2f\n",
                   successful, last_t - first_t,
                   successful > 1 && last_t > first_t ? (successful - 1) / (last_t - first_t) : 0.0);
        if (wrong) break;
    }

    if (successful > 1 && last_t > first_t)
        fps = (successful - 1) / (last_t - first_t);
    printf("[RESULT] resolution=%ux%u format=NV21 received=%d expected=%d ",
           OUT_W, OUT_H, successful, target_frames);
    printf("fps=%.3f errors=%d wrong_format=%d duplicate_pts=%d elapsed=%.3f\n",
           fps, failed, wrong, duplicate, last_t - first_t);
    result = (successful == target_frames && failed == 0 && wrong == 0 &&
              duplicate == 0 && fps >= 29.0) ? 0 : 1;
    printf("[RESULT] %s (acceptance: 300 consecutive 1080p NV21 frames, >=29.0 FPS measured)\n",
           result == 0 ? "PASS" : "FAIL");

cleanup:
    if (vi_bound) SAMPLE_COMM_VI_UnBind_VPSS(0, 0, VPSS_GRP_ID);
    if (grp_started) CVI_VPSS_StopGrp(VPSS_GRP_ID);
    if (ch_enabled) CVI_VPSS_DisableChn(VPSS_GRP_ID, VPSS_CHN_ID);
    if (vpss_created) CVI_VPSS_DestroyGrp(VPSS_GRP_ID);
    if (vi_ready) {
        SAMPLE_COMM_VI_DestroyIsp(&vi_cfg);
        SAMPLE_COMM_VI_DestroyVi(&vi_cfg);
        SAMPLE_COMM_VI_CLOSE();
    }
    if (output_pool != VB_INVALID_POOLID) CVI_VB_DestroyPool(output_pool);
    if (sys_ready) SAMPLE_COMM_SYS_Exit();
    return result;
}
