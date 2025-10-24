#include <cuda_runtime.h>

#include <iostream>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

extern "C" void launch_merge_kernel(uint8_t*, uint8_t*, uint8_t*, int, int, int,
                                    int);



struct Decoder {
  AVFormatContext* fmt_ctx;
  AVCodecContext* dec_ctx;
  int stream_index;
};

static AVBufferRef* hw_device_ctx = nullptr;

// HW device 초기화
int init_hw_device() {
  cudaSetDevice(0);  // 첫 번째 GPU 사용
  return av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, "0",
                                nullptr, 0);
}

// GPU 디코더 열기
Decoder* open_decoder(const char* url) {
  Decoder* dec = new Decoder();
  dec->fmt_ctx = nullptr;

  if (avformat_open_input(&dec->fmt_ctx, url, nullptr, nullptr) < 0)
    return nullptr;
  if (avformat_find_stream_info(dec->fmt_ctx, nullptr) < 0) return nullptr;

  dec->stream_index =
      av_find_best_stream(dec->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  AVStream* stream = dec->fmt_ctx->streams[dec->stream_index];

  const AVCodec* codec = avcodec_find_decoder_by_name("h264_cuvid");
  dec->dec_ctx = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(dec->dec_ctx, stream->codecpar);
  dec->dec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
  dec->dec_ctx->pkt_timebase = stream->time_base;

  dec->dec_ctx->get_format =
      [](AVCodecContext * ctx,
         const enum AVPixelFormat* pix_fmts) -> enum AVPixelFormat {
    for (int i = 0; pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
      if (pix_fmts[i] == AV_PIX_FMT_CUDA) return AV_PIX_FMT_CUDA;
    }
    return pix_fmts[0];
  };


  if (avcodec_open2(dec->dec_ctx, codec, nullptr) < 0) return nullptr;
  return dec;
}

// GPU 인코더 열기
AVCodecContext* open_encoder(int width, int height, int bitrate) {
  const AVCodec* enc = avcodec_find_encoder_by_name("h264_nvenc");
  AVCodecContext* enc_ctx = avcodec_alloc_context3(enc);
  enc_ctx->width = width;
  enc_ctx->height = height;
  enc_ctx->time_base = AVRational{1, 30};
  enc_ctx->framerate = AVRational{30, 1};
  enc_ctx->pix_fmt = AV_PIX_FMT_CUDA;
  enc_ctx->bit_rate = bitrate;
  enc_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);



   // hw_frames_ctx 생성
  AVBufferRef* hw_frames_ref = av_hwframe_ctx_alloc(enc_ctx->hw_device_ctx);
  AVHWFramesContext* frames_ctx = (AVHWFramesContext*)hw_frames_ref->data;
  frames_ctx->format = AV_PIX_FMT_CUDA;     // GPU format
  frames_ctx->sw_format = AV_PIX_FMT_NV12;  // 디코더 GPU format
  frames_ctx->width = width;
  frames_ctx->height = height;
  av_hwframe_ctx_init(hw_frames_ref);
  enc_ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);



  auto ret = avcodec_open2(enc_ctx, enc, nullptr);
  if ( ret < 0)
    return nullptr;
  return enc_ctx;
}

// decoded frame: CUDA -> compose -> encode
int main_mytest() {
  av_log_set_level(AV_LOG_INFO);
  avformat_network_init();
  if (init_hw_device() < 0) return -1;

  Decoder* dec1 = open_decoder("c:\\frameCount.mp4");
  Decoder* dec2 = open_decoder("c:\\frameCount2.mp4");
  AVCodecContext* enc = open_encoder(1920 , 1080, 4000000);

  AVFrame* frame1 = av_frame_alloc();
  AVFrame* frame2 = av_frame_alloc();
  AVFrame* merged = av_frame_alloc();

  merged->width = 1920 ;
  merged->height = 1080;
  merged->format = AV_PIX_FMT_CUDA;
  av_hwframe_get_buffer(enc->hw_frames_ctx, merged, 0);

  AVPacket pkt1, pkt2;
  av_init_packet(&pkt1);
  av_init_packet(&pkt2);

  int ret;

  while (true) {
    if (av_read_frame(dec1->fmt_ctx, &pkt1) < 0 ||
        av_read_frame(dec2->fmt_ctx, &pkt2) < 0)
      break;

    // Invalid pkt_timebase 방지
    if (pkt1.pts == AV_NOPTS_VALUE) pkt1.pts = 0;
    if (pkt1.dts == AV_NOPTS_VALUE) pkt1.dts = 0;
    if (pkt2.pts == AV_NOPTS_VALUE) pkt2.pts = 0;
    if (pkt2.dts == AV_NOPTS_VALUE) pkt2.dts = 0;


    avcodec_send_packet(dec1->dec_ctx, &pkt1);
    avcodec_send_packet(dec2->dec_ctx, &pkt2);
    ret = avcodec_receive_frame(dec1->dec_ctx, frame1);
        if (ret == 0) {
          printf("Frame received successfully\n");
        } else if(ret != AVERROR(EAGAIN)) {
          // FFmpeg 오류 코드를 문자열로 변환
          printf("avcodec_receive_frame failed: %d\n", (ret));
        }
    avcodec_receive_frame(dec2->dec_ctx, frame2);



    launch_merge_kernel((uint8_t*)merged->data[0], 
        (uint8_t*)frame1->data[0],
                        (uint8_t*)frame2->data[0],
                        1920 , 1080, merged->linesize[0],
                        frame1->linesize[0]);

    avcodec_send_frame(enc, merged);
    AVPacket outPkt;
    av_init_packet(&outPkt);
    while (avcodec_receive_packet(enc, &outPkt) == 0) {
      fwrite(outPkt.data, 1, outPkt.size, stdout);
      av_packet_unref(&outPkt);
    }

    av_packet_unref(&pkt1);
    av_packet_unref(&pkt2);
  }

  // Cleanup
  avcodec_free_context(&dec1->dec_ctx);
  avformat_close_input(&dec1->fmt_ctx);
  delete dec1;

  avcodec_free_context(&dec2->dec_ctx);
  avformat_close_input(&dec2->fmt_ctx);
  delete dec2;

  avcodec_free_context(&enc);
  av_buffer_unref(&hw_device_ctx);

  av_frame_free(&frame1);
  av_frame_free(&frame2);
  av_frame_free(&merged);

  return 0;
}













//decode example () 
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <iostream>

int main_decode_ok(int argc, char* argv[]) {
  //av_log_set_level(100);
  //const char* input_filename = "c:\\dev\\input.mp4";

  const char* input_filename = "c:\\frameCount.mp4";

  avformat_network_init();

  // 1. 입력 파일 열기
  AVFormatContext* fmt_ctx = nullptr;
  if (avformat_open_input(&fmt_ctx, input_filename, nullptr, nullptr) < 0) {
    std::cerr << "Failed to open input file\n";
    return -1;
  }

  if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
    std::cerr << "Failed to retrieve stream info\n";
    return -1;
  }

  // 2. 비디오 스트림 찾기
  int video_stream_index = -1;
  for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_stream_index = i;
      break;
    }
  }

  if (video_stream_index == -1) {
    std::cerr << "No video stream found\n";
    return -1;
  }

  // 3. 디코더 설정 (h264_cuvid)
  const AVCodec* decoder = avcodec_find_decoder_by_name("h264_cuvid");
  if (!decoder) {
    std::cerr << "h264_cuvid decoder not found\n";
    return -1;
  }

  AVCodecContext* codec_ctx = avcodec_alloc_context3(decoder);
  avcodec_parameters_to_context(codec_ctx,
                                fmt_ctx->streams[video_stream_index]->codecpar);

  // 4. CUDA 디바이스 초기화
  AVBufferRef* hw_device_ctx = nullptr;
  if (av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, nullptr,
                             nullptr, 0) < 0) {
    std::cerr << "Failed to create CUDA device context\n";
    return -1;
  }
  codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

  // 5. 디코더 열기
  if (avcodec_open2(codec_ctx, decoder, nullptr) < 0) {
    std::cerr << "Failed to open decoder\n";
    return -1;
  }

  // 6. 패킷과 프레임 준비
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();

  // 7. 디코딩 루프
  while (av_read_frame(fmt_ctx, pkt) >= 0) {
    if (pkt->stream_index == video_stream_index) {
      if (avcodec_send_packet(codec_ctx, pkt) == 0) {
        while (avcodec_receive_frame(codec_ctx, frame) == 0) {
          // 프레임은 GPU 메모리에 있음 (AV_PIX_FMT_CUDA)
          std::cout << "Decoded frame PTS: " << frame->pts << "\n";
          av_frame_unref(frame);  // discard
        }
      }
    }
    av_packet_unref(pkt);
  }

  // 8. 종료 처리
  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&codec_ctx);
  avformat_close_input(&fmt_ctx);
  av_buffer_unref(&hw_device_ctx);
  avformat_network_deinit();

  return 0;
}









// 전역 컨텍스트
AVFormatContext* fmt_ctx = nullptr;
AVCodecContext* dec_ctx = nullptr;
AVCodecContext* enc_ctx = nullptr;
AVFormatContext* out_fmt_ctx = nullptr;
AVStream* out_stream = nullptr;
//AVBufferRef* hw_device_ctx = nullptr;
AVBufferRef* hw_frames_ctx = nullptr;
int video_stream_index = -1;
void open_input_and_decoder(const char* input_filename) {
  avformat_open_input(&fmt_ctx, input_filename, nullptr, nullptr);
  avformat_find_stream_info(fmt_ctx, nullptr);

  //int video_stream_index = -1;
  for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_stream_index = i;
      break;
    }
  }

  const AVCodec* decoder = avcodec_find_decoder_by_name("h264_cuvid");
  dec_ctx = avcodec_alloc_context3(decoder);
  avcodec_parameters_to_context(dec_ctx,
                                fmt_ctx->streams[video_stream_index]->codecpar);
  av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, nullptr,
                         nullptr, 0);
  dec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
  avcodec_open2(dec_ctx, decoder, nullptr);
  dec_ctx->time_base = fmt_ctx->streams[video_stream_index]->time_base;
}

void open_encoder_and_output(const char* output_filename) {
  const AVCodec* encoder = avcodec_find_encoder_by_name("hevc_nvenc");
  enc_ctx = avcodec_alloc_context3(encoder);
  /*enc_ctx->width = 1920;
  enc_ctx->height = 1080;*/

  enc_ctx->width = dec_ctx->width;
  enc_ctx->height= dec_ctx->height;
  enc_ctx->pix_fmt = AV_PIX_FMT_CUDA;
  enc_ctx->time_base = dec_ctx->time_base;
  enc_ctx->framerate = {30, 1};
  enc_ctx->bit_rate = 4000000;
  enc_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

  hw_frames_ctx = av_hwframe_ctx_alloc(hw_device_ctx);
  AVHWFramesContext* frames_ctx = (AVHWFramesContext*)hw_frames_ctx->data;
  frames_ctx->format = AV_PIX_FMT_CUDA;
  frames_ctx->sw_format = AV_PIX_FMT_NV12;
   //AV_PIX_FMT_NV12;
  frames_ctx->width = enc_ctx->width;
  frames_ctx->height = enc_ctx->height;
  frames_ctx->initial_pool_size = 10;
  frames_ctx->device_ref = av_buffer_ref(hw_device_ctx);
  av_hwframe_ctx_init(hw_frames_ctx);
  enc_ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ctx);

  avcodec_open2(enc_ctx, encoder, nullptr);

  avformat_alloc_output_context2(&out_fmt_ctx, nullptr, nullptr,
                                 output_filename);
  out_stream = avformat_new_stream(out_fmt_ctx, nullptr);
  avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);
  out_stream->time_base = dec_ctx->time_base;
  avio_open(&out_fmt_ctx->pb, output_filename, AVIO_FLAG_WRITE);
  avformat_write_header(out_fmt_ctx, nullptr);
}



extern "C" void launch_copy_kernel(uint8_t* dst, int dst_pitch, int dst_x,
                                   int dst_y, 
                                   const uint8_t* src, int src_pitch, int width,
                                   int height);

void decode_encode_loop() {
  AVPacket* pkt = av_packet_alloc();
  AVFrame* dec_frame = av_frame_alloc();
  AVFrame* mid_frame = av_frame_alloc();
  int frame_index = 0;
  int ret;
  while (av_read_frame(fmt_ctx, pkt) >= 0) {
    if (pkt->stream_index == video_stream_index) {
      if (avcodec_send_packet(dec_ctx, pkt) == 0) {
        while (avcodec_receive_frame(dec_ctx, dec_frame) == 0) {
          mid_frame->format = AV_PIX_FMT_CUDA;
          mid_frame->width = enc_ctx->width;
          mid_frame->height = enc_ctx->height;
          mid_frame->hw_frames_ctx = enc_ctx->hw_frames_ctx;
          av_hwframe_get_buffer(enc_ctx->hw_frames_ctx, mid_frame, 0);

          AVFrame* mapped_src = av_frame_alloc();
          AVFrame* mapped_dst = av_frame_alloc();
          av_hwframe_map(mapped_src, dec_frame, AV_HWFRAME_MAP_READ);
          av_hwframe_map(mapped_dst, mid_frame, AV_HWFRAME_MAP_WRITE);

           launch_copy_kernel(mapped_dst->data[0], mapped_dst->linesize[0],
                0, 0,  
                             mapped_src->data[0], mapped_src->linesize[0],
                             dec_frame->width, dec_frame->height);

        /*  CUdeviceptr src_ptr = (CUdeviceptr)mapped_src->data[0];
          CUdeviceptr dst_ptr = (CUdeviceptr)mapped_dst->data[0];
          int src_pitch = mapped_src->linesize[0];
          int dst_pitch = mapped_dst->linesize[0];
          int copy_width = dec_frame->width;
          int copy_height = dec_frame->height;

          dim3 block(16, 16);
          dim3 grid((copy_width + 15) / 16, (copy_height + 15) / 16);
          copy_kernel<<<grid, block>>>((uint8_t*)dst_ptr, dst_pitch,
                                       (uint8_t*)src_ptr, src_pitch, copy_width,
                                       copy_height);
          cudaDeviceSynchronize();*/

          av_frame_free(&mapped_src);
          av_frame_free(&mapped_dst);

          mid_frame->pts = frame_index++;
          ret = avcodec_send_frame(enc_ctx, mid_frame);
          while (ret >= 0) {
            AVPacket* enc_pkt = av_packet_alloc();
            ret = avcodec_receive_packet(enc_ctx, enc_pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
              av_packet_free(&enc_pkt);
              break;
            } else if (ret < 0) {
              av_packet_free(&enc_pkt);
              break;
            }
            //cnt_encoded++;
            /*av_packet_rescale_ts(enc_pkt, dec_ctx->time_base,
                                 out_stream->time_base);*/
            enc_pkt->stream_index = out_stream->index;
            av_interleaved_write_frame(out_fmt_ctx, enc_pkt);
            av_packet_free(&enc_pkt);
          }

          av_frame_unref(dec_frame);
          av_frame_unref(mid_frame);
        }
      }
    }
    av_packet_unref(pkt);
  }

  av_write_trailer(out_fmt_ctx);
  av_frame_free(&dec_frame);
  av_frame_free(&mid_frame);
  av_packet_free(&pkt);
}

int cnt_read = 0;
int cnt_decoded = 0;
int cnt_send= 0;
int cnt_encoded= 0;

void decode_encode_loop2() {
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  AVFrame* hw_frame = av_frame_alloc();

  while (av_read_frame(fmt_ctx, pkt) >= 0) {
    if (pkt->stream_index == video_stream_index) {
      std::cout << "cnt:" <<cnt_decoded << " " << cnt_send << " " << cnt_encoded << "\n";
      int ret = avcodec_send_packet(dec_ctx, pkt);
      if (ret < 0) {
        av_packet_unref(pkt);
        continue;
      }

      while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
          break;
        } else if (ret < 0) {
          break;
        }
        cnt_decoded++;
#if 0
        // GPU 프레임으로 변환
        ret = av_hwframe_transfer_data(hw_frame, frame, 0);
        if (ret < 0) {
          break;
        }

        hw_frame->pts = frame->pts;

        // 인코더에 프레임 전송
        ret = avcodec_send_frame(enc_ctx, hw_frame);
        if (ret < 0) {
          break;
        }
#else
        ret = avcodec_send_frame(enc_ctx, frame);
        if (ret < 0) {
          break;
        }
        cnt_send++;
#endif
        // 인코딩된 패킷 수신 및 출력
        while (ret >= 0) {
          AVPacket* enc_pkt = av_packet_alloc();
          ret = avcodec_receive_packet(enc_ctx, enc_pkt);
          if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&enc_pkt);
            break;
          } else if (ret < 0) {
            av_packet_free(&enc_pkt);
            break;
          }
          cnt_encoded++;
          av_packet_rescale_ts(enc_pkt, enc_ctx->time_base,
                               out_stream->time_base);
          enc_pkt->stream_index = out_stream->index;
          av_interleaved_write_frame(out_fmt_ctx, enc_pkt);
          av_packet_free(&enc_pkt);
        }
      }
    }
    av_packet_unref(pkt);
  }

  // 플러시
  avcodec_send_frame(enc_ctx, nullptr);
  while (true) {
    AVPacket* enc_pkt = av_packet_alloc();
    int ret = avcodec_receive_packet(enc_ctx, enc_pkt);
    if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
      av_packet_free(&enc_pkt);
      break;
    }
    av_packet_rescale_ts(enc_pkt, enc_ctx->time_base, out_stream->time_base);
    enc_pkt->stream_index = out_stream->index;
    av_interleaved_write_frame(out_fmt_ctx, enc_pkt);
    av_packet_free(&enc_pkt);
  }

  av_write_trailer(out_fmt_ctx);
  av_frame_free(&frame);
  av_frame_free(&hw_frame);
  av_packet_free(&pkt);
}




void decode_encode_loop3() {
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  int ret;
  while (av_read_frame(fmt_ctx, pkt) >= 0) {
    cnt_read++;
    if (pkt->stream_index == video_stream_index) {
      std::cout << "cnt:" << cnt_read << " " << cnt_decoded << " " << cnt_send << " "
                << cnt_encoded << "\n";
      if (avcodec_send_packet(dec_ctx, pkt) == 0) {
        while (avcodec_receive_frame(dec_ctx, frame) == 0) {
          // 프레임은 GPU 메모리에 있음 (AV_PIX_FMT_CUDA)
          //std::cout << "Decoded frame PTS: " << frame->pts << "\n";

          ret = avcodec_send_frame(enc_ctx, frame);
          std::cout << "ret:" << ret << "\n";

          // 인코딩된 패킷 수신 및 출력
          while (ret >= 0) {
            AVPacket* enc_pkt = av_packet_alloc();
            ret = avcodec_receive_packet(enc_ctx, enc_pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
              av_packet_free(&enc_pkt);
              break;
            } else if (ret < 0) {
              av_packet_free(&enc_pkt);
              break;
            }
            cnt_encoded++;
            /*av_packet_rescale_ts(enc_pkt, enc_ctx->time_base,
                                 out_stream->time_base);*/
            enc_pkt->stream_index = out_stream->index;
            av_interleaved_write_frame(out_fmt_ctx, enc_pkt);
            av_packet_free(&enc_pkt);
          }

          av_frame_unref(frame);  // discard
        }
      }
    }
    av_packet_unref(pkt);
  }

  // 플러시
  avcodec_send_frame(enc_ctx, nullptr);
  while (true) {
    AVPacket* enc_pkt = av_packet_alloc();
    int ret = avcodec_receive_packet(enc_ctx, enc_pkt);
    if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
      av_packet_free(&enc_pkt);
      break;
    }
    /*av_packet_rescale_ts(enc_pkt, enc_ctx->time_base, out_stream->time_base);*/
    enc_pkt->stream_index = out_stream->index;
    av_interleaved_write_frame(out_fmt_ctx, enc_pkt);
    av_packet_free(&enc_pkt);
  }

  av_write_trailer(out_fmt_ctx);
  av_frame_free(&frame);
  av_packet_free(&pkt);
}





void open4_encoder_and_output(const char* output_filename) {
  const AVCodec* encoder = avcodec_find_encoder_by_name("hevc_nvenc");
  enc_ctx = avcodec_alloc_context3(encoder);
  /*enc_ctx->width = 1920;
  enc_ctx->height = 1080;*/

  enc_ctx->width = dec_ctx->width*2;
  enc_ctx->height = dec_ctx->height*2;
  enc_ctx->pix_fmt = AV_PIX_FMT_CUDA;
  enc_ctx->time_base = {1, 30};
  enc_ctx->framerate = {30, 1};
  enc_ctx->bit_rate = 4000000;
  enc_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

  hw_frames_ctx = av_hwframe_ctx_alloc(hw_device_ctx);
  AVHWFramesContext* frames_ctx = (AVHWFramesContext*)hw_frames_ctx->data;
  frames_ctx->format = AV_PIX_FMT_CUDA;
  frames_ctx->sw_format = AV_PIX_FMT_NV12;
  // AV_PIX_FMT_NV12;
  frames_ctx->width = enc_ctx->width;
  frames_ctx->height = enc_ctx->height;
  frames_ctx->initial_pool_size = 60;
  frames_ctx->device_ref = av_buffer_ref(hw_device_ctx);
  av_hwframe_ctx_init(hw_frames_ctx);
  enc_ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ctx);

  avcodec_open2(enc_ctx, encoder, nullptr);

  avformat_alloc_output_context2(&out_fmt_ctx, nullptr, nullptr,
                                 output_filename);
  out_stream = avformat_new_stream(out_fmt_ctx, nullptr);
  avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);
  out_stream->time_base = enc_ctx->time_base;
  avio_open(&out_fmt_ctx->pb, output_filename, AVIO_FLAG_WRITE);
  avformat_write_header(out_fmt_ctx, nullptr);
}


void decode_encode_loop4() {
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  int ret;
  std::cout << "dec.timebase:" << dec_ctx->time_base.num << "/" << dec_ctx->time_base.den << "\n";
  while (av_read_frame(fmt_ctx, pkt) >= 0) {
    
    if (pkt->stream_index == video_stream_index) {
      cnt_read++;
      std::cout << "cnt:" << cnt_read << " " << cnt_decoded << " " << cnt_send
                << " " << cnt_encoded << "\n";
      if (avcodec_send_packet(dec_ctx, pkt) == 0) {
        while (avcodec_receive_frame(dec_ctx, frame) == 0) {

          cnt_decoded++;
          AVFrame* padded_frame = av_frame_alloc();
          padded_frame->format = AV_PIX_FMT_CUDA;
          padded_frame->width = enc_ctx->width;
          padded_frame->height = enc_ctx->height;
          padded_frame->hw_frames_ctx = av_buffer_ref(enc_ctx->hw_frames_ctx);
          av_hwframe_get_buffer(enc_ctx->hw_frames_ctx, padded_frame, 0);

          launch_copy_kernel(padded_frame->data[0], padded_frame->linesize[0],
            100, 100,
                             frame->data[0], frame->linesize[0], 
                             frame->width,
                             frame->height);
          launch_copy_kernel(padded_frame->data[1], padded_frame->linesize[1],
          100, 50,
                             frame->data[1], frame->linesize[1], frame->width,
                             frame->height / 2);

 

          //padded_frame->pts = frame->pts;
          padded_frame->pts = cnt_decoded;



          // 프레임은 GPU 메모리에 있음 (AV_PIX_FMT_CUDA)
          std::cout << "Decoded frame PTS: " << frame->pts << "\n";

          ret = avcodec_send_frame(enc_ctx, padded_frame);
          std::cout << "ret:" << ret << "\n";

          // 인코딩된 패킷 수신 및 출력
          while (ret >= 0) {
            AVPacket* enc_pkt = av_packet_alloc();
            ret = avcodec_receive_packet(enc_ctx, enc_pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
              av_packet_free(&enc_pkt);
              break;
            } else if (ret < 0) {
              av_packet_free(&enc_pkt);
              break;
            }
            cnt_encoded++;
            av_packet_rescale_ts(enc_pkt, enc_ctx->time_base,
                                 out_stream->time_base);
            enc_pkt->stream_index = out_stream->index;
            av_interleaved_write_frame(out_fmt_ctx, enc_pkt);
            av_packet_free(&enc_pkt);
          }

          av_frame_unref(frame);  // discard
          av_frame_free(&padded_frame);
        }
      }
    }
    av_packet_unref(pkt);
  }

  // 플러시
  avcodec_send_frame(enc_ctx, nullptr);
  while (true) {
    AVPacket* enc_pkt = av_packet_alloc();
    int ret = avcodec_receive_packet(enc_ctx, enc_pkt);
    if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
      av_packet_free(&enc_pkt);
      break;
    }

     cnt_encoded++;
    std::cout << "flushing:" << cnt_encoded << "\n";

    av_packet_rescale_ts(enc_pkt, enc_ctx->time_base,
     out_stream->time_base);
    enc_pkt->stream_index = out_stream->index;
    av_interleaved_write_frame(out_fmt_ctx, enc_pkt);
    av_packet_free(&enc_pkt);
  }

  av_write_trailer(out_fmt_ctx);
  av_frame_free(&frame);
  av_packet_free(&pkt);
}

#define TEST_PADDED
// "c:\\dev\\frameCount.mp4"; temp ok
// input.mp4: failed


int main() {
  //av_log_set_level(100);
  av_log_set_level(40);
  //const char* input_filename = "c:\\dev\\frameCount.mp4";
  //const char* input_filename = "c:\\dev\\frameCount30s_nv12.mp4";
  //const char* input_filename = "c:\\dev\\input.mp4"; 
  const char* output_filename = "c:\\dev\\output.mp4";

  open_input_and_decoder(input_filename);
#if defined(TEST_PADDED)
  open4_encoder_and_output(output_filename);
  decode_encode_loop4();
#else
  open_encoder_and_output(output_filename);
  //decode_encode_loop();
  //decode_encode_loop2();

  //// 끊어져보인다. timescale 문제??
  decode_encode_loop3();
  #endif
  // 정리
  avcodec_free_context(&dec_ctx);
  avcodec_free_context(&enc_ctx);
  avformat_close_input(&fmt_ctx);
  avformat_free_context(out_fmt_ctx);
  av_buffer_unref(&hw_device_ctx);
  av_buffer_unref(&hw_frames_ctx);

  return 0;
}
