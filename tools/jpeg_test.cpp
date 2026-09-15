// Hardware JPEG snapshot test: one NV12 frame from /dev/video0 -> VE JPEG -> file.
// Same zero-copy flow as userspace/src/encoder.cpp, with VENC_CODEC_JPEG.
#include <ar0234/v4l2.hpp>

#include <memoryAdapter.h>
#include <vencoder.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>

using namespace std::chrono_literals;

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : "/tmp/snap.jpg";
	const int quality = argc > 2 ? atoi(argv[2]) : 90;

	ar0234::CaptureConfig cc{};
	cc.size = {1920, 1080};
	cc.fps = 30;
	ar0234::Capture cap{cc};
	if (!cap.dmabuf_exported()) {
		printf("FAIL: dmabuf export unavailable\n");
		return 1;
	}
	auto bufs = cap.buffers();
	cap.start();

	auto fr = cap.dequeue(2s);
	if (!fr) {
		printf("FAIL: no frame within 2s\n");
		return 1;
	}
	auto &buf = bufs[fr->index()];
	const unsigned w = cap.size().width, h = cap.size().height;
	const std::uint32_t luma = w * h;
	printf("frame seq=%u bytes=%zu\n", fr->sequence(), buf.data().size());

	auto *memops = MemAdapterGetOpsS();
	if (!memops || CdcMemOpen(memops) < 0) {
		printf("FAIL: MemAdapter open\n");
		return 1;
	}

	VencBaseConfig base{};
	base.memops = memops;
	base.nInputWidth = w;
	base.nInputHeight = h;
	base.nStride = w;
	base.nDstWidth = w;
	base.nDstHeight = h;
	base.eInputFormat = VENC_PIXEL_YUV420SP;

	VideoEncoder *enc = VideoEncCreate(VENC_CODEC_JPEG);
	if (!enc) {
		printf("FAIL: VideoEncCreate(JPEG)\n");
		return 1;
	}
	int q = quality;
	int rc = VideoEncSetParameter(enc, VENC_IndexParamJpegQuality, &q);
	printf("SetParameter(quality=%d) rc=%d\n", quality, rc);
	if (VideoEncInit(enc, &base)) {
		printf("FAIL: VideoEncInit\n");
		return 1;
	}

	user_iommu_param iova{};
	iova.fd = buf.dmabuf.get();
	VideoEncoderGetVeIommuAddr(enc, &iova);
	if (!iova.iommu_addr) {
		printf("FAIL: GetVeIommuAddr\n");
		return 1;
	}

	VencInputBuffer in{};
	in.nID = 0;
	in.pAddrVirY = buf.data().data();
	in.pAddrVirC = buf.data().data() + luma;
	in.pAddrPhyY = reinterpret_cast<unsigned char *>(static_cast<unsigned long>(iova.iommu_addr));
	in.pAddrPhyC = reinterpret_cast<unsigned char *>(static_cast<unsigned long>(iova.iommu_addr + luma));
	in.nShareBufFd = -1;
	AddOneInputBuffer(enc, &in);
	if (VideoEncodeOneFrame(enc)) {
		printf("FAIL: VideoEncodeOneFrame\n");
		return 1;
	}
	AlreadyUsedInputBuffer(enc, &in);

	std::ofstream f{out, std::ios::binary};
	std::size_t total = 0;
	VencOutputBuffer ob{};
	while (GetOneBitstreamFrame(enc, &ob) == 0) {
		f.write(reinterpret_cast<const char *>(ob.pData0), ob.nSize0);
		total += ob.nSize0;
		if (ob.nSize1) {
			f.write(reinterpret_cast<const char *>(ob.pData1), ob.nSize1);
			total += ob.nSize1;
		}
		FreeOneBitStreamFrame(enc, &ob);
	}
	f.close();
	VideoEncoderFreeVeIommuAddr(enc, &iova);
	VideoEncUnInit(enc);
	VideoEncDestroy(enc);
	CdcMemClose(memops);

	printf("%s: %s (%zu bytes)\n", total ? "OK" : "FAIL: empty stream", out, total);
	return total ? 0 : 1;
}
