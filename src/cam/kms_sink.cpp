/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2021, Ideas on Board Oy
 *
 * kms_sink.cpp - KMS Sink
 */

#include "kms_sink.h"

#include <array>
#include <algorithm>
#include <assert.h>
#include <iostream>
#include <memory>
#include <stdint.h>
#include <string.h>

#include <libcamera/camera.h>
#include <libcamera/formats.h>
#include <libcamera/framebuffer.h>
#include <libcamera/stream.h>

#include "drm.h"

KMSSink::KMSSink(const std::string &connectorName)
	: connector_(nullptr), crtc_(nullptr), plane_(nullptr), mode_(nullptr)
{
	int ret = dev_.init();
	if (ret < 0)
		return;

	/*
	 * Find the requested connector. If no specific connector is requested,
	 * pick the first connected connector or, if no connector is connected,
	 * the first connector with unknown status.
	 */
	for (const DRM::Connector &conn : dev_.connectors()) {
		if (!connectorName.empty()) {
			if (conn.name() != connectorName)
				continue;

			connector_ = &conn;
			break;
		}

		if (conn.status() == DRM::Connector::Connected) {
			connector_ = &conn;
			break;
		}

		if (!connector_ && conn.status() == DRM::Connector::Unknown)
			connector_ = &conn;
	}

	if (!connector_) {
		if (!connectorName.empty())
			std::cerr
				<< "Connector " << connectorName << " not found"
				<< std::endl;
		else
			std::cerr << "No connected connector found" << std::endl;
		return;
	}

	dev_.requestComplete.connect(this, &KMSSink::requestComplete);
}

void KMSSink::mapBuffer(libcamera::FrameBuffer *buffer)
{
	std::array<uint32_t, 4> strides = {};

	/* \todo Should libcamera report per-plane strides ? */
	unsigned int uvStrideMultiplier;

	switch (format_) {
	case libcamera::formats::NV24:
	case libcamera::formats::NV42:
		uvStrideMultiplier = 4;
		break;
	case libcamera::formats::YUV420:
	case libcamera::formats::YVU420:
	case libcamera::formats::YUV422:
		uvStrideMultiplier = 1;
		break;
	default:
		uvStrideMultiplier = 2;
		break;
	}

	strides[0] = stride_;
	for (unsigned int i = 1; i < buffer->planes().size(); ++i)
		strides[i] = stride_ * uvStrideMultiplier / 2;

	std::unique_ptr<DRM::FrameBuffer> drmBuffer =
		dev_.createFrameBuffer(*buffer, format_, size_, strides);
	if (!drmBuffer)
		return;

	buffers_.emplace(std::piecewise_construct,
			 std::forward_as_tuple(buffer),
			 std::forward_as_tuple(std::move(drmBuffer)));
}

int KMSSink::configure(const libcamera::CameraConfiguration &config)
{
	if (!connector_)
		return -EINVAL;

	crtc_ = nullptr;
	plane_ = nullptr;
	mode_ = nullptr;

	const libcamera::StreamConfiguration &cfg = config.at(0);

	const std::vector<DRM::Mode> &modes = connector_->modes();
	const auto iter = std::find_if(modes.begin(), modes.end(),
				       [&](const DRM::Mode &mode) {
					       return mode.hdisplay == cfg.size.width &&
						      mode.vdisplay == cfg.size.height;
				       });
	if (iter == modes.end()) {
		std::cerr
			<< "No mode matching " << cfg.size.toString()
			<< std::endl;
		return -EINVAL;
	}

	int ret = configurePipeline(cfg.pixelFormat);
	if (ret < 0)
		return ret;

	mode_ = &*iter;
	size_ = cfg.size;
	stride_ = cfg.stride;

	return 0;
}

int KMSSink::selectPipeline(const libcamera::PixelFormat &format)
{
	/*
	 * If the requested format has an alpha channel, also consider the X
	 * variant.
	 */
	libcamera::PixelFormat xFormat;

	switch (format) {
	case libcamera::formats::ABGR8888:
		xFormat = libcamera::formats::XBGR8888;
		break;
	case libcamera::formats::ARGB8888:
		xFormat = libcamera::formats::XRGB8888;
		break;
	case libcamera::formats::BGRA8888:
		xFormat = libcamera::formats::BGRX8888;
		break;
	case libcamera::formats::RGBA8888:
		xFormat = libcamera::formats::RGBX8888;
		break;
	}

	/*
	 * Find a CRTC and plane suitable for the request format and the
	 * connector at the end of the pipeline. Restrict the search to primary
	 * planes for now.
	 */
	for (const DRM::Encoder *encoder : connector_->encoders()) {
		for (const DRM::Crtc *crtc : encoder->possibleCrtcs()) {
			for (const DRM::Plane *plane : crtc->planes()) {
				if (plane->type() != DRM::Plane::TypePrimary)
					continue;

				if (plane->supportsFormat(format)) {
					crtc_ = crtc;
					plane_ = plane;
					format_ = format;
					return 0;
				}

				if (plane->supportsFormat(xFormat)) {
					crtc_ = crtc;
					plane_ = plane;
					format_ = xFormat;
					return 0;
				}
			}
		}
	}

	return -EPIPE;
}

int KMSSink::configurePipeline(const libcamera::PixelFormat &format)
{
	const int ret = selectPipeline(format);
	if (ret) {
		std::cerr
			<< "Unable to find display pipeline for format "
			<< format.toString() << std::endl;

		return ret;
	}

	std::cout
		<< "Using KMS plane " << plane_->id() << ", CRTC " << crtc_->id()
		<< ", connector " << connector_->name()
		<< " (" << connector_->id() << ")" << std::endl;

	return 0;
}

int KMSSink::start()
{
	std::unique_ptr<DRM::AtomicRequest> request;

	int ret = FrameSink::start();
	if (ret < 0)
		return ret;

	/* Disable all CRTCs and planes to start from a known valid state. */
	request = std::make_unique<DRM::AtomicRequest>(&dev_);

	for (const DRM::Crtc &crtc : dev_.crtcs())
		request->addProperty(&crtc, "ACTIVE", 0);

	for (const DRM::Plane &plane : dev_.planes()) {
		request->addProperty(&plane, "CRTC_ID", 0);
		request->addProperty(&plane, "FB_ID", 0);
	}

	ret = request->commit(DRM::AtomicRequest::FlagAllowModeset);
	if (ret < 0) {
		std::cerr
			<< "Failed to disable CRTCs and planes: "
			<< strerror(-ret) << std::endl;
		return ret;
	}

	return 0;
}

int KMSSink::stop()
{
	/* Display pipeline. */
	DRM::AtomicRequest request(&dev_);

	request.addProperty(connector_, "CRTC_ID", 0);
	request.addProperty(crtc_, "ACTIVE", 0);
	request.addProperty(crtc_, "MODE_ID", 0);
	request.addProperty(plane_, "CRTC_ID", 0);
	request.addProperty(plane_, "FB_ID", 0);

	int ret = request.commit(DRM::AtomicRequest::FlagAllowModeset);
	if (ret < 0) {
		std::cerr
			<< "Failed to stop display pipeline: "
			<< strerror(-ret) << std::endl;
		return ret;
	}

	/* Free all buffers. */
	pending_.reset();
	queued_.reset();
	active_.reset();
	buffers_.clear();

	return FrameSink::stop();
}

bool KMSSink::processRequest(libcamera::Request *camRequest)
{
	/*
	 * Perform a very crude rate adaptation by simply dropping the request
	 * if the display queue is full.
	 */
	if (pending_)
		return true;

	libcamera::FrameBuffer *buffer = camRequest->buffers().begin()->second;
	auto iter = buffers_.find(buffer);
	if (iter == buffers_.end())
		return true;

	DRM::FrameBuffer *drmBuffer = iter->second.get();

	unsigned int flags = DRM::AtomicRequest::FlagAsync;
	DRM::AtomicRequest *drmRequest = new DRM::AtomicRequest(&dev_);
	drmRequest->addProperty(plane_, "FB_ID", drmBuffer->id());

	if (!active_ && !queued_) {
		/* Enable the display pipeline on the first frame. */
		drmRequest->addProperty(connector_, "CRTC_ID", crtc_->id());

		drmRequest->addProperty(crtc_, "ACTIVE", 1);
		drmRequest->addProperty(crtc_, "MODE_ID", mode_->toBlob(&dev_));

		drmRequest->addProperty(plane_, "CRTC_ID", crtc_->id());
		drmRequest->addProperty(plane_, "SRC_X", 0 << 16);
		drmRequest->addProperty(plane_, "SRC_Y", 0 << 16);
		drmRequest->addProperty(plane_, "SRC_W", mode_->hdisplay << 16);
		drmRequest->addProperty(plane_, "SRC_H", mode_->vdisplay << 16);
		drmRequest->addProperty(plane_, "CRTC_X", 0);
		drmRequest->addProperty(plane_, "CRTC_Y", 0);
		drmRequest->addProperty(plane_, "CRTC_W", mode_->hdisplay);
		drmRequest->addProperty(plane_, "CRTC_H", mode_->vdisplay);

		flags |= DRM::AtomicRequest::FlagAllowModeset;
	}

	pending_ = std::make_unique<Request>(drmRequest, camRequest);

	std::lock_guard<std::mutex> lock(lock_);

	if (!queued_) {
		int ret = drmRequest->commit(flags);
		if (ret < 0) {
			std::cerr
				<< "Failed to commit atomic request: "
				<< strerror(-ret) << std::endl;
			/* \todo Implement error handling */
		}

		queued_ = std::move(pending_);
	}

	return false;
}

void KMSSink::requestComplete(DRM::AtomicRequest *request)
{
	std::lock_guard<std::mutex> lock(lock_);

	assert(queued_ && queued_->drmRequest_.get() == request);

	/* Complete the active request, if any. */
	if (active_)
		requestProcessed.emit(active_->camRequest_);

	/* The queued request becomes active. */
	active_ = std::move(queued_);

	/* Queue the pending request, if any. */
	if (pending_) {
		pending_->drmRequest_->commit(DRM::AtomicRequest::FlagAsync);
		queued_ = std::move(pending_);
	}
}
