#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>


#define MAX_FDS_OPEN 512

// Only invariants are allowed to be static.
static int32_t inputWidth = 0;
static int32_t inputHeight = 0;
static enum AVPixelFormat inputPixelFormat = AV_PIX_FMT_NONE;
static size_t inputBytesPerPixel = 0;


static void sigterm_handler(int sig) {
  (void)sig; // Supress unused warning.
  exit(123);
}


static size_t pix_fmt_to_bytes_per_pixel(const char* pix_fmt) {
  // Count the number of '8' in pix_fmt.
  size_t result = 0;
  size_t len = strlen(pix_fmt);
  for (size_t i = 0; i < len; ++i) {
    if (pix_fmt[i] == '8') ++result;
  }
  return result;
}


static enum AVPixelFormat pix_fmt_str_to_enum(const char* pix_fmt) {
  if (strcmp(pix_fmt, "RGB888") == 0) {
    return AV_PIX_FMT_RGB24;
  } else if (strcmp(pix_fmt, "BGR888") == 0) {
    return AV_PIX_FMT_BGR24;
  } else if (strcmp(pix_fmt, "ABGR8888") == 0) {
    return AV_PIX_FMT_ABGR;
  } else if (strcmp(pix_fmt, "ARGB8888") == 0) {
    return AV_PIX_FMT_ARGB;
  } else if (strcmp(pix_fmt, "BGRA8888") == 0) {
    return AV_PIX_FMT_BGRA;
  } else if (strcmp(pix_fmt, "RGBA8888") == 0) {
    return AV_PIX_FMT_RGBA;
  } else {
    printf("Error! Invalid PIX_FMT: %s\n", pix_fmt);
    exit(1);
  }
}


static void read_picture(AVPicture* picture) {
  char header[4];
  memset(&header, 0, sizeof(header));

  if (fread(&header, sizeof(header), 1, stdin) == 1) {
    if (strncmp(header, "FRM\n", 4) != 0) {
      fprintf(stderr, "invalid header: %s\n", header);
      exit(1);
    }

    size_t pictureSize = inputWidth * inputHeight * inputBytesPerPixel;
    if (fread(picture->data[0], 1, pictureSize, stdin) != pictureSize) {
      perror("unable to read frame");
      exit(1);
    }
  } else {
    perror("unable to read header");
    exit(1);
  }
}

/**
  If function returns 0 it is up to the caller to free the packet.
**/
static int encode_picture(AVCodecContext *encodingContext,
                          AVPicture *picture,
                          AVPacket *packet) {
  static struct AVFrame *frame = NULL;
  static struct SwsContext *sws_ctx = NULL;

  if (!frame) {
    frame = avcodec_alloc_frame();
    if (!frame) {
      fprintf(stderr, "error allocating video frame\n");
      exit(1);
    }
    frame->format = encodingContext->pix_fmt;
    frame->width  = encodingContext->width;
    frame->height = encodingContext->height;

    // The image can be allocated by any means and av_image_alloc()
    // is just the most convenient way if av_malloc() is to be used.
    if (av_image_alloc(frame->data, frame->linesize, frame->width,
                       frame->height, frame->format, 32) < 0) {
      fprintf(stderr, "error allocation video frame data\n");
      exit(1);
    }
  }

  if (!sws_ctx) {
    sws_ctx = sws_getContext(inputWidth, inputHeight, inputPixelFormat,
                             frame->width, frame->height, frame->format,
                             SWS_BICUBIC, NULL, NULL, NULL);
    if (!sws_ctx) {
      fprintf(stderr, "Could not initialize the conversion context\n");
      exit(1);
    }
  }

  if (sws_scale(sws_ctx, (const uint8_t * const *)picture->data, picture->linesize,
                0, inputHeight, frame->data, frame->linesize) <= 0) {
    fprintf(stderr, "unable to rescale image\n");
    exit(1);
  }

  frame->pts = av_gettime();

  // Encode the image
  int got_packet = 0;
  if (avcodec_encode_video2(encodingContext, packet, frame, &got_packet) < 0) {
    fprintf(stderr, "error encoding video frame\n");
    return -1;
  } else if (got_packet && packet->size) {
    return 0;
  } else {
    return 1;
  }
}


static void send_packet(AVFormatContext *outputContext, AVPacket* packet) {
  /* If size is zero, it means the image was buffered. */
  packet->stream_index = 0;

  // Write the compressed frame to the media output
  int err = av_write_frame(outputContext, packet);
  if (err < 0) {
    fprintf(stderr, "unable to write frame: %s\n", av_err2str(err));
    return;
  }

  // Force flushing the output context
  av_write_frame(outputContext, NULL);

  // Free packet allocated by decoding.
  av_free_packet(packet);
}


int main(int argc, char const *argv[]) {
  // Check for parameters
  if (argc < 6) {
    fprintf(stderr, "%s DEST_IP DEST_PORT WIDTH HEIGHT PIX_FMT\n", argv[0]);
    return 1;
  }

  // Close all file descriptors except the standard ones
  // This avoids conflitcs between parent context and this one.
  for (int i = STDERR_FILENO + 1; i < MAX_FDS_OPEN; ++i) {
    close(i);
  }

  // Register a few signals to avoid blocking forever.
  signal(SIGINT, sigterm_handler);
  signal(SIGTERM, sigterm_handler);

  // Initialize global parameters
  char outputAddr[256] = {0};
  snprintf(outputAddr, sizeof(outputAddr), "udp://%s:%s", argv[1], argv[2]);
  inputWidth = atoi(argv[3]);
  inputHeight = atoi(argv[4]);
  inputPixelFormat = pix_fmt_str_to_enum(argv[5]);
  inputBytesPerPixel = pix_fmt_to_bytes_per_pixel(argv[5]);

  // Register all formats and codecs
  av_register_all();
  avformat_network_init();

  // Alloc context for outputting the data.
  AVFormatContext *outputContext = NULL;
  avformat_alloc_output_context2(&outputContext, NULL, "mpegts", outputAddr);
  if (!outputContext) {
    fprintf(stderr, "error allocating output context\n");
    return 1;
  }

  // Find the H.264 encoder. The `encoder` struct must be "opened" before using.
  AVCodec *encoder = avcodec_find_encoder_by_name("libx264");
  if (!encoder) {
    fprintf(stderr, "x264 encoder not found\n");
    return 1;
  }

  // Add an stream to the output. This stream will contain video frames.
  AVStream *stream = avformat_new_stream(outputContext, encoder);
  if (!stream) {
    fprintf(stderr, "error when creating stream\n");
    return 1;
  }
  // Configure the stream ID (needed for transmitting it)
  stream->id = outputContext->nb_streams - 1;

  // Grab the encoding context from format.
  AVCodecContext *encodingContext = stream->codec;

  // Resolution must be a multiple of two
  encodingContext->width = inputWidth;
  encodingContext->height = inputHeight;
  // Set default encoding parameters
  encodingContext->time_base.num = 1;
  encodingContext->time_base.den = 15;
  encodingContext->gop_size = 0; // Emit only intra frames
  encodingContext->has_b_frames = 0; // We don't want b frames
  encodingContext->me_method = 1; // No motion estimation
  encodingContext->pix_fmt = AV_PIX_FMT_YUV420P;

  // Set the same presets as in the command line
  av_opt_set(encodingContext->priv_data, "preset", "ultrafast", 0);
  av_opt_set(encodingContext->priv_data, "tune", "zerolatency", 0);
  av_opt_set_double(encodingContext->priv_data, "crf", 20.0, 0);

  // Open encoding context for our encoder
  if (avcodec_open2(encodingContext, encoder, NULL) < 0) {
    fprintf(stderr, "error opening encoder\n");
    return 1;
  }

  // Open output buffer. This will also open the UDP socket.
  if (avio_open(&outputContext->pb, outputAddr, AVIO_FLAG_WRITE) < 0) {
    fprintf(stderr, "error opening output buffer\n");
    return 1;
  }

  // Write transport stream header (PAT, PMT, etc).
  // This segfaults without avio_open.
  if (avformat_write_header(outputContext, NULL) < 0) {
    fprintf(stderr, "error writing mpegts header\n");
    return 1;
  }

  // Allocate picture so it can be correcly aligned.
  AVPicture *inputPicture = calloc(1, sizeof(AVPicture));
  if (av_image_alloc(inputPicture->data, inputPicture->linesize,
                     inputWidth, inputHeight, inputPixelFormat, 32) < 0) {
    fprintf(stderr, "error allocating input picture\n");
    return 1;
  }

  while (1) {
    read_picture(inputPicture);

    AVPacket packet;
    memset(&packet, 0, sizeof(packet));
    if (encode_picture(encodingContext, inputPicture, &packet) == 0) {
      send_packet(outputContext, &packet);
    }
  }
}
