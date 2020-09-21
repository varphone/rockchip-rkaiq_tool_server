#include "rkaiq_raw_protocol.h"
#include "multiframe_process.h"

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "rkaiq_raw_protocol.cpp"

static int capture_status = READY;
static int capture_mode = CAPTURE_NORMAL;
static int capture_frames = 0;
static int capture_frames_index = 0;
static uint16_t capture_check_sum;
static struct capture_info cap_info;
static uint32_t *averge_frame0;
static uint16_t *averge_frame1;

extern std::string g_sensor_name;

static void RunCmd(const char *cmd, char *result) {
  FILE *fp;
  fp = popen(cmd, "r");
  if (!fp) {
    LOG_INFO("[%s] fail\n", cmd);
    return;
  }
  fgets(result, 1024, fp);
  LOG_INFO("fgets result: %s\n", result);
  fclose(fp);
}

static int SetLHcg(int mode) {
  char cmd[1024] = {0};
  char result[1024] = {0};
  int pos = g_sensor_name.find(" ");
  snprintf(cmd, sizeof(cmd),
           "echo %d > /sys/class/i2c-dev/i2c-%d/device/%s/cam_s_cg", mode,
           atoi(g_sensor_name.substr(pos + 1, pos + 2).c_str()),
           g_sensor_name.substr(pos + 1, g_sensor_name.size() - 1).c_str());
  RunCmd(cmd, result);
  return 0;
}

static void InitCommandPingAns(CommandData_t *cmd, int ret_status) {
  strncpy((char *)cmd->RKID, TAG_DEVICE_TO_PC, sizeof(cmd->RKID));
  cmd->cmdType = DEVICE_TO_PC;
  cmd->cmdID = CMD_ID_CAPTURE_STATUS;
  cmd->datLen = 1;
  memset(cmd->dat, 0, sizeof(cmd->dat));
  cmd->dat[0] = ret_status;
  cmd->checkSum = 0;
  for (int i = 0; i < cmd->datLen; i++)
    cmd->checkSum += cmd->dat[i];
}

static void InitCommandRawCapAns(CommandData_t *cmd, int ret_status) {
  strncpy((char *)cmd->RKID, TAG_DEVICE_TO_PC, sizeof(cmd->RKID));
  cmd->cmdType = DEVICE_TO_PC;
  cmd->cmdID = CMD_ID_CAPTURE_RAW_CAPTURE;
  cmd->datLen = 2;
  memset(cmd->dat, 0, sizeof(cmd->dat));
  cmd->dat[0] = 0x00; // ProcessID
  cmd->dat[1] = ret_status;
  cmd->checkSum = 0;
  for (int i = 0; i < cmd->datLen; i++)
    cmd->checkSum += cmd->dat[i];
}

static void RawCaptureinit(CommandData_t *cmd) {
  char *buf = (char *)(cmd->dat);
  Capture_Reso_t *Reso = (Capture_Reso_t *)(cmd->dat + 1);
  int ret = initCamHwInfos(&cap_info);
  if (cap_info.link == link_to_isp) {
    ret = setupLink(&cap_info, true);

    if (ret < 0)
      LOG_INFO(">>>>>>>>>setup link to isp output raw failed\n");
  }
  cap_info.dev_fd = -1;
  cap_info.subdev_fd = -1;
  if (cap_info.link == link_to_isp) {
    cap_info.dev_name = cap_info.vd_path.isp_main_path;
  } else {
    cap_info.dev_name = cap_info.cif_path.cif_video_path;
  }

  cap_info.io = IO_METHOD_MMAP;
  cap_info.height = Reso->height;
  cap_info.width = Reso->width;
  // cap_info.format = v4l2_fourcc('B', 'G', '1', '2');
  LOG_INFO("get ResW: %d  ResH: %d\n", cap_info.width, cap_info.height);
}

static void RawCaptureDeinit() {
  if (cap_info.subdev_fd > 0) {
    device_close(cap_info.subdev_fd);
    cap_info.subdev_fd = -1;
    LOG_ERROR("device_close(cap_info.subdev_fd)\n");
  }
  if (cap_info.dev_fd > 0) {
    device_close(cap_info.dev_fd);
    cap_info.dev_fd = -1;
    LOG_ERROR("device_close(cap_info.dev_fd)\n");
  }
}

static void GetSensorPara(CommandData_t *cmd, int ret_status) {
  struct v4l2_queryctrl ctrl;
  struct v4l2_subdev_frame_interval finterval;
  struct v4l2_subdev_format fmt;
  struct v4l2_format format;

  memset(cmd, 0, sizeof(CommandData_t));

  Sensor_Params_t *sensorParam = (Sensor_Params_t *)(&cmd->dat[1]);
  int hblank, vblank;
  int vts, hts, ret;
  float fps;
  int endianness = 0;

  // cap_info.dev_fd = device_open(cap_info.dev_name);
  cap_info.subdev_fd = device_open(cap_info.sd_path.device_name);

  LOG_INFO("sensor subdev path: %s\n", cap_info.sd_path.device_name);

  memset(&ctrl, 0, sizeof(ctrl));
  ctrl.id = V4L2_CID_HBLANK;
  if (device_getblank(cap_info.subdev_fd, &ctrl) < 0) {
    // todo
    sensorParam->status = RES_FAILED;
    goto end;
  }
  hblank = ctrl.minimum;
  LOG_INFO("get hblank: %d\n", hblank);

  memset(&ctrl, 0, sizeof(ctrl));
  ctrl.id = V4L2_CID_VBLANK;
  if (device_getblank(cap_info.subdev_fd, &ctrl) < 0) {
    // todo
    sensorParam->status = RES_FAILED;
    goto end;
  }
  vblank = ctrl.minimum;
  LOG_INFO("get vblank: %d\n", vblank);

  memset(&fmt, 0, sizeof(fmt));
  fmt.pad = 0;
  fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
  if (device_getsubdevformat(cap_info.subdev_fd, &fmt) < 0) {
    sensorParam->status = RES_FAILED;
    goto end;
  }
  vts = vblank + fmt.format.height;
  hts = hblank + fmt.format.width;
  LOG_INFO("get hts: %d  vts: %d\n", hts, vts);
  cap_info.format = convert_to_v4l2fmt(&cap_info, fmt.format.code);
  cap_info.sd_path.sen_fmt = fmt.format.code;
  cap_info.sd_path.width = fmt.format.width;
  cap_info.sd_path.height = fmt.format.height;

  LOG_INFO("get sensor code: %d  bits: %d, cap_info.format:  %d\n",
           cap_info.sd_path.sen_fmt, cap_info.sd_path.bits, cap_info.format);

  /* set isp subdev fmt to bayer raw*/
  if (cap_info.link == link_to_isp) {
    ret = rkisp_set_ispsd_fmt(&cap_info, fmt.format.width, fmt.format.height,
                              fmt.format.code, cap_info.width, cap_info.height,
                              fmt.format.code);
    endianness = 1;
    LOG_INFO("rkisp_set_ispsd_fmt: %d endianness = %d\n", ret, endianness);

    if (ret) {
      LOG_ERROR("subdev choose the best fit fmt: %dx%d, 0x%08x\n",
                fmt.format.width, fmt.format.height, fmt.format.code);
      sensorParam->status = RES_FAILED;
      goto end;
    }
  } else if (cap_info.link == link_to_vicap) {
    ret = system(VICAP_COMPACT_TEST_OFF);
    LOG_INFO("VICAP_COMPACT_TEST: %d  change to no compact\n", ret);
  }

  memset(&finterval, 0, sizeof(finterval));
  finterval.pad = 0;
  if (device_getsensorfps(cap_info.subdev_fd, &finterval) < 0) {
    sensorParam->status = RES_FAILED;
    goto end;
  }
  fps = (float)(finterval.interval.denominator) / finterval.interval.numerator;
  LOG_INFO("get fps: %f\n", fps);

  strncpy((char *)cmd->RKID, TAG_DEVICE_TO_PC, sizeof(cmd->RKID));
  cmd->cmdType = DEVICE_TO_PC;
  cmd->cmdID = CMD_ID_CAPTURE_RAW_CAPTURE;
  cmd->datLen = 14;
  memset(cmd->dat, 0, sizeof(cmd->dat));
  cmd->dat[0] = 0x01;
  sensorParam->status = ret_status;
  sensorParam->fps = fps;
  sensorParam->hts = hts;
  sensorParam->vts = vts;
  sensorParam->bits = cap_info.sd_path.bits;
  sensorParam->endianness = endianness;
  LOG_INFO("sensorParam->endianness: %d\n", endianness);

  cmd->checkSum = 0;
  for (int i = 0; i < cmd->datLen; i++) {
    cmd->checkSum += cmd->dat[i];
  }
  LOG_INFO("cmd->checkSum %d\n", cmd->checkSum);

  if (cap_info.subdev_fd > 0) {
    device_close(cap_info.subdev_fd);
    cap_info.subdev_fd = -1;
  }
  if (cap_info.dev_fd > 0) {
    device_close(cap_info.dev_fd);
    cap_info.dev_fd = -1;
  }
  return;

end:
  strncpy((char *)cmd->RKID, TAG_DEVICE_TO_PC, sizeof(cmd->RKID));
  cmd->cmdType = PC_TO_DEVICE;
  cmd->cmdID = CMD_ID_CAPTURE_RAW_CAPTURE;
  cmd->datLen = 14;
  cmd->dat[0] = 0x01;
  cmd->checkSum = 0;
  for (int i = 0; i < cmd->datLen; i++)
    cmd->checkSum += cmd->dat[i];
  if (cap_info.subdev_fd > 0) {
    device_close(cap_info.subdev_fd);
    cap_info.subdev_fd = -1;
  }
}

static void SetCapConf(CommandData_t *recv_cmd, CommandData_t *cmd,
                       int ret_status) {
  memset(cmd, 0, sizeof(CommandData_t));
  Capture_Params_t *CapParam = (Capture_Params_t *)(recv_cmd->dat + 1);

  for (int i = 0; i < recv_cmd->datLen; i++) {
    LOG_INFO("data[%d]: 0x%x\n", i, recv_cmd->dat[i]);
  }

  LOG_INFO(" set gain        : %d\n", CapParam->gain);
  LOG_INFO(" set exposure    : %d\n", CapParam->time);
  LOG_INFO(" set lhcg        : %d\n", CapParam->lhcg);
  LOG_INFO(" set bits        : %d\n", CapParam->bits);
  LOG_INFO(" set framenumber : %d\n", CapParam->framenumber);
  LOG_INFO(" set multiframe  : %d\n", CapParam->multiframe);
  cap_info.subdev_fd = device_open(cap_info.sd_path.device_name);

  capture_frames = CapParam->framenumber;
  capture_frames_index = 0;
  cap_info.frame_count = CapParam->framenumber;
  cap_info.lhcg = CapParam->lhcg;
  capture_mode = CapParam->multiframe;
  capture_check_sum = 0;

  struct v4l2_control exp;
  exp.id = V4L2_CID_EXPOSURE;
  exp.value = CapParam->time;
  struct v4l2_control gain;
  gain.id = V4L2_CID_ANALOGUE_GAIN;
  gain.value = CapParam->gain;

  if (device_setctrl(cap_info.subdev_fd, &exp) < 0) {
    LOG_ERROR(" set exposure result failed to device\n");
    ret_status = RES_FAILED;
  }
  if (device_setctrl(cap_info.subdev_fd, &gain) < 0) {
    LOG_ERROR(" set gain result failed to device\n");
    ret_status = RES_FAILED;
  }

  strncpy((char *)cmd->RKID, TAG_DEVICE_TO_PC, sizeof(cmd->RKID));
  cmd->cmdType = DEVICE_TO_PC;
  cmd->cmdID = CMD_ID_CAPTURE_RAW_CAPTURE;
  cmd->datLen = 2;
  memset(cmd->dat, 0, sizeof(cmd->dat));
  cmd->dat[0] = 0x02;
  cmd->dat[1] = ret_status;
  for (int i = 0; i < cmd->datLen; i++) {
    LOG_INFO("data[%d]: 0x%x\n", i, cmd->dat[i]);
  }
  cmd->checkSum = 0;
  for (int i = 0; i < cmd->datLen; i++) {
    cmd->checkSum += cmd->dat[i];
  }
  LOG_INFO("cmd->checkSum %d\n", cmd->checkSum);

  if (cap_info.subdev_fd > 0) {
    device_close(cap_info.subdev_fd);
    cap_info.subdev_fd = -1;
  }
}

static void SendRawData(int socket, int index, void *buffer, int size) {
  LOG_INFO(" SendRawData\n");
  char *buf = NULL;
  int total = size;
  int packet_len = MAXPACKETSIZE;
  int send_size = 0;
  int ret_val;
  uint16_t check_sum = 0;
  buf = (char *)buffer;
  while (total > 0) {
    if (total < packet_len) {
      send_size = total;
    } else {
      send_size = packet_len;
    }
    ret_val = send(socket, buf, send_size, 0);
    total -= send_size;
    buf += ret_val;
  }

  buf = (char *)buffer;
  for (int i = 0; i < size; i++) {
    check_sum += buf[i];
  }

  LOG_INFO("capture raw index %d, check_sum %d capture_check_sum %d\n", index,
           check_sum, capture_check_sum);
  capture_check_sum = check_sum;
}

static void DoCaptureCallBack(int socket, int index, void *buffer, int size) {
  LOG_INFO(" DoCaptureCallBack size %d\n", size);
  int width = cap_info.width;
  int height = cap_info.height;
  if (size > (width * height * 2)) {
    LOG_ERROR(" DoMultiFrameCallBack size error\n");
    return;
  }
  SendRawData(socket, index, buffer, size);
}

static void DoCapture(int socket) {
  LOG_INFO("DoCapture entry!!!!!\n");
  AutoDuration ad;
  int skip_frame = 5;

  if (capture_frames_index == 0) {
    for (int i = 0; i < skip_frame; i++) {
      if (i == 0 && cap_info.lhcg != 2)
        SetLHcg(cap_info.lhcg);
      read_frame(socket, i, &cap_info, nullptr);
      LOG_INFO("DoCapture skip frame %d ...\n", i);
    }
  }

  read_frame(socket, capture_frames_index, &cap_info, DoCaptureCallBack);
  capture_frames_index++;

  LOG_INFO("DoCapture %lld ms %lld us\n", ad.Get() / 1000, ad.Get() % 1000);
  LOG_INFO("DoCapture exit!!!!!\n");
}

static void DoMultiFrameCallBack(int socket, int index, void *buffer,
                                 int size) {
  LOG_INFO(" DoMultiFrameCallBack size %d\n", size);
  AutoDuration ad;
  int width = cap_info.width;
  int height = cap_info.height;

  if (size > (width * height * 2)) {
    LOG_ERROR(" DoMultiFrameCallBack size error\n");
    return;
  }

  DumpRawData((uint16_t *)buffer, size, 2);
  if (cap_info.link == link_to_vicap)
    MultiFrameAddition(averge_frame0, (uint16_t *)buffer, width, height, false);
  else
    MultiFrameAddition(averge_frame0, (uint16_t *)buffer, width, height);
  DumpRawData32(averge_frame0, size, 2);
  LOG_INFO("index %d MultiFrameAddition %lld ms %lld us\n", index,
           ad.Get() / 1000, ad.Get() % 1000);
  ad.Reset();
  if (index == (capture_frames - 1)) {
    MultiFrameAverage(averge_frame0, averge_frame1, width, height,
                      capture_frames);
    DumpRawData32(averge_frame0, size, 2);
    DumpRawData(averge_frame1, size, 2);
    LOG_INFO("index %d MultiFrameAverage %lld ms %lld us\n", index,
             ad.Get() / 1000, ad.Get() % 1000);
    ad.Reset();
    SendRawData(socket, index, averge_frame1, size);
    LOG_INFO("index %d SendRawData %lld ms %lld us\n", index, ad.Get() / 1000,
             ad.Get() % 1000);
  } else if (index == ((capture_frames >> 1) - 1)) {
    SendRawData(socket, index, buffer, size);
    LOG_INFO("index %d SendRawData %lld ms %lld us\n", index, ad.Get() / 1000,
             ad.Get() % 1000);
  }
}

static int InitMultiFrame() {
  uint32_t one_frame_size = cap_info.width * cap_info.height * sizeof(uint32_t);
  averge_frame0 = (uint32_t *)malloc(one_frame_size);
  one_frame_size = one_frame_size >> 1;
  averge_frame1 = (uint16_t *)malloc(one_frame_size);
  memset(averge_frame0, 0, one_frame_size);
  memset(averge_frame1, 0, one_frame_size);
  return 0;
}

static int deInitMultiFrame() {
  if (averge_frame0 != nullptr) {
    free(averge_frame0);
  }
  if (averge_frame1 != nullptr) {
    free(averge_frame1);
  }
  averge_frame0 = nullptr;
  averge_frame1 = nullptr;
  return 0;
}

static void DoMultiFrameCapture(int socket) {
  LOG_INFO("DoMultiFrameCapture entry!!!!!\n");
  AutoDuration ad;

  int skip_frame = 5;
  if (capture_frames_index == 0) {
    for (int i = 0; i < skip_frame; i++) {
      if (i == 0 && cap_info.lhcg != 2)
        SetLHcg(cap_info.lhcg);
      read_frame(socket, i, &cap_info, nullptr);
      LOG_INFO("DoCapture skip frame %d ...\n", i);
    }
  }

  if (capture_frames_index == 0) {
    for (int i = 0; i < (capture_frames >> 1); i++) {
      read_frame(socket, i, &cap_info, DoMultiFrameCallBack);
      capture_frames_index++;
    }
  } else if (capture_frames_index == (capture_frames >> 1)) {
    for (int i = (capture_frames >> 1); i < capture_frames; i++) {
      read_frame(socket, i, &cap_info, DoMultiFrameCallBack);
      capture_frames_index++;
    }
  }

  LOG_INFO("DoMultiFrameCapture %lld ms %lld us\n", ad.Get() / 1000,
           ad.Get() % 1000);
  LOG_INFO("DoMultiFrameCapture exit!!!!!\n");
}

static void DumpCapinfo() {
  LOG_DEBUG("DumpCapinfo: \n");
  LOG_DEBUG("    dev_name ------------- %s\n", cap_info.dev_name);
  LOG_DEBUG("    dev_fd --------------- %d\n", cap_info.dev_fd);
  LOG_DEBUG("    io ------------------- %d\n", cap_info.io);
  LOG_DEBUG("    width ---------------- %d\n", cap_info.width);
  LOG_DEBUG("    height --------------- %d\n", cap_info.height);
  LOG_DEBUG("    format --------------- %d\n", cap_info.format);
  LOG_DEBUG("    capture_buf_type ----- %d\n", cap_info.capture_buf_type);
  LOG_DEBUG("    out_file ------------- %s\n", cap_info.out_file);
  LOG_DEBUG("    frame_count ---------- %d\n", cap_info.frame_count);
}

static int StartCapture() {
  LOG_INFO(" enter\n");
  init_device(&cap_info);
  DumpCapinfo();
  start_capturing(&cap_info);
  if (capture_mode != CAPTURE_NORMAL)
    InitMultiFrame();
  LOG_INFO(" exit\n");
  return 0;
}

static int StopCapture() {
  LOG_INFO(" enter\n");
  stop_capturing(&cap_info);
  uninit_device(&cap_info);
  RawCaptureDeinit();
  if (capture_mode != CAPTURE_NORMAL)
    deInitMultiFrame();
  LOG_INFO(" exit\n");
  return 0;
}

static void RawCaputure(CommandData_t *cmd, int socket) {
  LOG_INFO(" enter\n");
  LOG_INFO("capture_frames %d capture_frames_index %d\n", capture_frames,
           capture_frames_index);
  if (capture_frames_index == 0) {
    StartCapture();
  }

  if (capture_mode == CAPTURE_NORMAL)
    DoCapture(socket);
  else
    DoMultiFrameCapture(socket);

  if (capture_frames_index == capture_frames) {
    StopCapture();
  }

  LOG_INFO(" exit\n");
}

static void SendRawDataResult(CommandData_t *cmd, CommandData_t *recv_cmd) {
  unsigned short *checksum;
  checksum = (unsigned short *)&recv_cmd->dat[1];
  strncpy((char *)cmd->RKID, TAG_DEVICE_TO_PC, sizeof(cmd->RKID));
  cmd->cmdType = DEVICE_TO_PC;
  cmd->cmdID = CMD_ID_CAPTURE_RAW_CAPTURE;
  cmd->datLen = 2;
  memset(cmd->dat, 0, sizeof(cmd->dat));
  cmd->dat[0] = 0x04;
  LOG_INFO("capture_check_sum %d, recieve %d\n", capture_check_sum, *checksum);
  if (capture_check_sum == *checksum) {
    cmd->dat[1] = RES_SUCCESS;
  } else {
    cmd->dat[1] = RES_FAILED;
    StopCapture();
  }
  cmd->checkSum = 0;
  for (int i = 0; i < cmd->datLen; i++)
    cmd->checkSum += cmd->dat[i];
}

void RKAiqRawProtocol::HandlerRawCapMessage(int sockfd, char *buffer,
                                            int size) {
  CommandData_t *common_cmd = (CommandData_t *)buffer;
  CommandData_t send_cmd;
  char send_data[MAXPACKETSIZE];
  int ret_val, ret;

  LOG_INFO("HandlerRawCapMessage:\n");

  // for (int i = 0; i < common_cmd->datLen; i++) {
  //   LOG_INFO("DATA[%d]: 0x%x\n", i, common_cmd->dat[i]);
  // }

  // if (strcmp((char *)common_cmd->RKID, TAG_PC_TO_DEVICE) == 0) {
  //   LOG_INFO("RKID: %s\n", common_cmd->RKID);
  // } else {
  //   LOG_INFO("RKID: Unknow\n");
  //   return;
  // }

  if (common_cmd->cmdType == CMD_TYPE_CAPTURE) {
    LOG_INFO("cmdType: CMD_TYPE_CAPTURE\n");
  } else {
    LOG_INFO("cmdType: Unknow\n");
    return;
  }

  LOG_INFO("cmdID: %d\n", common_cmd->cmdID);

  switch (common_cmd->cmdID) {
  case CMD_ID_CAPTURE_STATUS:
    LOG_INFO("CmdID CMD_ID_CAPTURE_STATUS in\n");
    if (common_cmd->dat[0] == KNOCK_KNOCK) {
      InitCommandPingAns(&send_cmd, READY);
      LOG_INFO("Device is READY\n");
    } else {
      LOG_ERROR("Unknow CMD_ID_CAPTURE_STATUS message\n");
    }
    memcpy(send_data, &send_cmd, sizeof(CommandData_t));
    send(sockfd, send_data, sizeof(CommandData_t), 0);
    LOG_INFO("cmdID CMD_ID_CAPTURE_STATUS out\n\n");
    break;
  case CMD_ID_CAPTURE_RAW_CAPTURE: {
    LOG_INFO("CmdID RAW_CAPTURE in\n");
    char *datBuf = (char *)(common_cmd->dat);

    switch (datBuf[0]) {
    case DATA_ID_CAPTURE_RAW_STATUS:
      LOG_INFO("ProcID DATA_ID_CAPTURE_RAW_STATUS in\n");
      if (common_cmd->dat[1] == KNOCK_KNOCK) {
        if (capture_status == RAW_CAP) {
          LOG_INFO("capture_status BUSY\n");
          InitCommandRawCapAns(&send_cmd, BUSY);
        } else {
          LOG_INFO("capture_status READY\n");
          InitCommandRawCapAns(&send_cmd, READY);
        }
      } else {
        LOG_ERROR("Unknow DATA_ID_CAPTURE_RAW_STATUS message\n");
      }
      memcpy(send_data, &send_cmd, sizeof(CommandData_t));
      send(sockfd, send_data, sizeof(CommandData_t), 0);
      LOG_INFO("ProcID DATA_ID_CAPTURE_RAW_STATUS out\n");
      break;
    case DATA_ID_CAPTURE_RAW_GET_PARAM:
      LOG_INFO("ProcID DATA_ID_CAPTURE_RAW_GET_PARAM in\n");
      RawCaptureinit(common_cmd);
      GetSensorPara(&send_cmd, RES_SUCCESS);
      LOG_INFO("send_cmd.checkSum %d\n", send_cmd.checkSum);
      memcpy(send_data, &send_cmd, sizeof(CommandData_t));
      ret_val = send(sockfd, send_data, sizeof(CommandData_t), 0);
      LOG_INFO("ProcID DATA_ID_CAPTURE_RAW_GET_PARAM out\n");
      break;
    case DATA_ID_CAPTURE_RAW_SET_PARAM:
      LOG_INFO("ProcID DATA_ID_CAPTURE_RAW_SET_PARAM in\n");
      SetCapConf(common_cmd, &send_cmd, READY);
      memcpy(send_data, &send_cmd, sizeof(CommandData_t));
      send(sockfd, send_data, sizeof(CommandData_t), 0);
      LOG_INFO("ProcID DATA_ID_CAPTURE_RAW_SET_PARAM out\n");
      break;
    case DATA_ID_CAPTURE_RAW_START: {
      LOG_INFO("ProcID DATA_ID_CAPTURE_RAW_START in\n");
      capture_status = RAW_CAP;
      RawCaputure(&send_cmd, sockfd);
      capture_status = AVALIABLE;
      LOG_INFO("ProcID DATA_ID_CAPTURE_RAW_START out\n");
      break;
    }
    case DATA_ID_CAPTURE_RAW_CHECKSUM:
      LOG_INFO("ProcID DATA_ID_CAPTURE_RAW_CHECKSUM in\n");
      SendRawDataResult(&send_cmd, common_cmd);
      memcpy(send_data, &send_cmd, sizeof(CommandData_t));
      ret_val = send(sockfd, send_data, sizeof(CommandData_t), 0);
      LOG_INFO("ProcID DATA_ID_CAPTURE_RAW_CHECKSUM out\n");
      break;
    default:
      break;
    }
    LOG_INFO("CmdID RAW_CAPTURE out\n\n");
    break;
  }
  default:
    break;
  }
}