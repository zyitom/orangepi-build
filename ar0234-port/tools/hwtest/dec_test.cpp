// Hardware H.264 decode test (libvdecoder): decode a raw Annex-B file, save the
// first luma plane, count frames.
//   dec_test /tmp/t30.h264 1920 1080
#include <vdecoder.h>
#include <vbasetype.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

int main(int argc, char **argv)
{
	if (argc < 4) {
		printf("usage: %s file.h264 width height\n", argv[0]);
		return 1;
	}
	const char *path = argv[1];
	const int w = atoi(argv[2]), h = atoi(argv[3]);

	std::ifstream in{path, std::ios::binary};
	if (!in) {
		printf("FAIL: open %s\n", path);
		return 1;
	}
	std::vector<char> data{(std::istreambuf_iterator<char>(in)),
			       std::istreambuf_iterator<char>()};
	printf("stream: %zu bytes\n", data.size());

	AddVDPlugin();	 // register codec plugins (libaw*.so) - without this
			 // VideoEngineCreate rejects every format
	VideoDecoder *dec = CreateVideoDecoder();
	if (!dec) {
		printf("FAIL: CreateVideoDecoder\n");
		return 1;
	}
	VideoStreamInfo si{};
	si.eCodecFormat = VIDEO_CODEC_FORMAT_H264;
	si.nWidth = w;
	si.nHeight = h;
	si.nFrameRate = 30;
	si.bIsFramePackage = 0;		 // raw Annex-B = stream package
	VConfig vc{};
	vc.nSupportMaxWidth = w;
	vc.nSupportMaxHeight = h;
	vc.sVcuConfig.bEnableVcu = 1;
	vc.sVcuConfig.bVcuAutoMode = 1;
	if (InitializeVideoDecoder(dec, &si, &vc)) {
		printf("FAIL: InitializeVideoDecoder\n");
		return 1;
	}

	std::size_t pos = 0;
	int id = 0, frames = 0, saved = 0, idle = 0;
	bool eos = false;
	std::ofstream out{"/tmp/dec_first.y", std::ios::binary};

	while (!eos || frames < 3) {
		if (pos < data.size()) {
			char *pbuf = nullptr;
			int bufsize = 0, ringbufsize = 0;
			char *ring = nullptr;
			std::size_t chunk = 32 * 1024;
			if (pos + chunk > data.size())
				chunk = data.size() - pos;
			if (RequestVideoStreamBuffer(dec, (int)chunk, &pbuf, &bufsize,
						     &ring, &ringbufsize, 0) == 0 && pbuf) {
				int n = (int)chunk < bufsize ? (int)chunk : bufsize;
				memcpy(pbuf, data.data() + pos, n);
				pos += n;
				VideoStreamDataInfo di{};
				di.pData = pbuf;
				di.nLength = n;
				di.nID = id++;
				di.bIsFirstPart = (id == 1);
				di.bIsLastPart = (pos >= data.size());
				if (SubmitVideoStreamData(dec, &di, 0)) {
					printf("FAIL: SubmitVideoStreamData\n");
					return 1;
				}
				if (pos >= data.size())
					eos = true;
			} else {
				eos = pos >= data.size();	// buffer full: go decode
			}
		} else if (!eos) {
			eos = true;
		}

		DecodeVideoStream(dec, eos ? 1 : 0, 0, 0, 0);

		VideoPicture *pic = RequestPicture(dec, 0);
		if (pic) {
			++frames;
			if (!saved && pic->pData0) {
				out.write(pic->pData0, (std::streamsize)pic->nLineStride * pic->nHeight);
				saved = 1;
				printf("first picture: %dx%d stride %d\n", pic->nWidth, pic->nHeight,
				       pic->nLineStride);
			}
			ReturnPicture(dec, pic);
		}
		if (!pic)
			++idle;
		if (idle > 300 || (eos && frames >= 1 && !pic))
			break;
	}
	out.close();
	DestroyVideoDecoder(dec);
	printf("%s: decoded %d frames, first luma -> /tmp/dec_first.y\n",
	       frames ? "OK" : "FAIL: no frames", frames);
	return frames ? 0 : 1;
}
