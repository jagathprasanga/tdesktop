/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_photo.h"

#include "data/data_session.h"
#include "data/data_file_origin.h"
#include "data/data_reply_preview.h"
#include "data/data_photo_media.h"
#include "ui/image/image.h"
#include "main/main_session.h"
#include "mainwidget.h"
#include "storage/file_download.h"
#include "core/application.h"
#include "facades.h"
#include "app.h"

namespace {

constexpr auto kPhotoSideLimit = 1280;

using Data::PhotoMedia;
using Data::PhotoSize;
using Data::PhotoSizeIndex;
using Data::kPhotoSizeCount;

} // namespace

PhotoData::PhotoData(not_null<Data::Session*> owner, PhotoId id)
: id(id)
, _owner(owner) {
}

PhotoData::~PhotoData() {
	for (auto &image : _images) {
		base::take(image.loader).reset();
	}
}

Data::Session &PhotoData::owner() const {
	return *_owner;
}

Main::Session &PhotoData::session() const {
	return _owner->session();
}

void PhotoData::automaticLoadSettingsChanged() {
	const auto index = PhotoSizeIndex(PhotoSize::Large);
	if (!(_images[index].flags & Data::CloudFile::Flag::Cancelled)) {
		return;
	}
	_images[index].loader = nullptr;
	_images[index].flags &= ~Data::CloudFile::Flag::Cancelled;
}

void PhotoData::load(
		Data::FileOrigin origin,
		LoadFromCloudSetting fromCloud,
		bool autoLoading) {
	load(PhotoSize::Large, origin, fromCloud, autoLoading);
}

bool PhotoData::loading() const {
	return loading(PhotoSize::Large);
}

int PhotoData::validSizeIndex(PhotoSize size) const {
	const auto index = PhotoSizeIndex(size);
	for (auto i = index; i != kPhotoSizeCount; ++i) {
		if (_images[i].location.valid()) {
			return i;
		}
	}
	return PhotoSizeIndex(PhotoSize::Large);
}

bool PhotoData::hasExact(PhotoSize size) const {
	return _images[PhotoSizeIndex(size)].location.valid();
}

bool PhotoData::loading(PhotoSize size) const {
	return (_images[validSizeIndex(size)].loader != nullptr);
}

bool PhotoData::failed(PhotoSize size) const {
	const auto flags = _images[validSizeIndex(size)].flags;
	return (flags & Data::CloudFile::Flag::Failed);
}

const ImageLocation &PhotoData::location(PhotoSize size) const {
	return _images[validSizeIndex(size)].location;
}

int PhotoData::SideLimit() {
	return kPhotoSideLimit;
}

std::optional<QSize> PhotoData::size(PhotoSize size) const {
	const auto &provided = location(size);
	const auto result = QSize{ provided.width(), provided.height() };
	const auto limit = SideLimit();
	if (result.isEmpty()) {
		return std::nullopt;
	} else if (result.width() <= limit && result.height() <= limit) {
		return result;
	}
	const auto scaled = result.scaled(limit, limit, Qt::KeepAspectRatio);
	return QSize(std::max(scaled.width(), 1), std::max(scaled.height(), 1));
}

int PhotoData::imageByteSize(PhotoSize size) const {
	return _images[validSizeIndex(size)].byteSize;
}

bool PhotoData::displayLoading() const {
	const auto index = PhotoSizeIndex(PhotoSize::Large);
	return _images[index].loader
		? (!_images[index].loader->loadingLocal()
			|| !_images[index].loader->autoLoading())
		: (uploading() && !waitingForAlbum());
}

void PhotoData::cancel() {
	if (loading()) {
		_images[PhotoSizeIndex(PhotoSize::Large)].loader->cancel();
	}
}

float64 PhotoData::progress() const {
	if (uploading()) {
		if (uploadingData->size > 0) {
			const auto result = float64(uploadingData->offset)
				/ uploadingData->size;
			return snap(result, 0., 1.);
		}
		return 0.;
	}
	const auto index = PhotoSizeIndex(PhotoSize::Large);
	return loading() ? _images[index].loader->currentProgress() : 0.;
}

bool PhotoData::cancelled() const {
	const auto index = PhotoSizeIndex(PhotoSize::Large);
	return (_images[index].flags & Data::CloudFile::Flag::Cancelled);
}

void PhotoData::setWaitingForAlbum() {
	if (uploading()) {
		uploadingData->waitingForAlbum = true;
	}
}

bool PhotoData::waitingForAlbum() const {
	return uploading() && uploadingData->waitingForAlbum;
}

int32 PhotoData::loadOffset() const {
	const auto index = PhotoSizeIndex(PhotoSize::Large);
	return loading() ? _images[index].loader->currentOffset() : 0;
}

bool PhotoData::uploading() const {
	return (uploadingData != nullptr);
}

Image *PhotoData::getReplyPreview(Data::FileOrigin origin) {
	if (!_replyPreview) {
		_replyPreview = std::make_unique<Data::ReplyPreview>(this);
	}
	return _replyPreview->image(origin);
}

void PhotoData::setRemoteLocation(
		int32 dc,
		uint64 access,
		const QByteArray &fileReference) {
	_fileReference = fileReference;
	if (_dc != dc || _access != access) {
		_dc = dc;
		_access = access;
	}
}

MTPInputPhoto PhotoData::mtpInput() const {
	return MTP_inputPhoto(
		MTP_long(id),
		MTP_long(_access),
		MTP_bytes(_fileReference));
}

QByteArray PhotoData::fileReference() const {
	return _fileReference;
}

void PhotoData::refreshFileReference(const QByteArray &value) {
	_fileReference = value;
	for (auto &image : _images) {
		image.location.refreshFileReference(value);
	}
}

void PhotoData::collectLocalData(not_null<PhotoData*> local) {
	if (local == this) {
		return;
	}

	for (auto i = 0; i != kPhotoSizeCount; ++i) {
		if (const auto from = local->_images[i].location.file().cacheKey()) {
			if (const auto to = _images[i].location.file().cacheKey()) {
				_owner->cache().copyIfEmpty(from, to);
			}
		}
	}
	if (const auto localMedia = local->activeMediaView()) {
		auto media = createMediaView();
		media->collectLocalData(localMedia.get());
		_owner->keepAlive(std::move(media));
	}
}

bool PhotoData::isNull() const {
	return !_images[PhotoSizeIndex(PhotoSize::Large)].location.valid();
}

void PhotoData::load(
		PhotoSize size,
		Data::FileOrigin origin,
		LoadFromCloudSetting fromCloud,
		bool autoLoading) {
	const auto index = validSizeIndex(size);
	auto &image = _images[index];

	// Could've changed, if the requested size didn't have a location.
	const auto loadingSize = static_cast<PhotoSize>(index);
	const auto cacheTag = Data::kImageCacheTag;
	Data::LoadCloudFile(image, origin, fromCloud, autoLoading, cacheTag, [=] {
		if (const auto active = activeMediaView()) {
			return !active->image(size);
		}
		return true;
	}, [=](QImage result) {
		if (const auto active = activeMediaView()) {
			active->set(loadingSize, std::move(result));
		}
		if (loadingSize == PhotoSize::Large) {
			_owner->photoLoadDone(this);
		}
	}, [=](bool started) {
		if (loadingSize == PhotoSize::Large) {
			_owner->photoLoadFail(this, started);
		}
	}, [=] {
		if (loadingSize == PhotoSize::Large) {
			_owner->photoLoadProgress(this);
		}
	});

	if (size == PhotoSize::Large) {
		_owner->notifyPhotoLayoutChanged(this);
	}
}

std::shared_ptr<PhotoMedia> PhotoData::createMediaView() {
	if (auto result = activeMediaView()) {
		return result;
	}
	auto result = std::make_shared<PhotoMedia>(this);
	_media = result;
	return result;
}

std::shared_ptr<PhotoMedia> PhotoData::activeMediaView() const {
	return _media.lock();
}

void PhotoData::updateImages(
		const QByteArray &inlineThumbnailBytes,
		const ImageWithLocation &small,
		const ImageWithLocation &thumbnail,
		const ImageWithLocation &large) {
	if (!inlineThumbnailBytes.isEmpty()
		&& _inlineThumbnailBytes.isEmpty()) {
		_inlineThumbnailBytes = inlineThumbnailBytes;
	}
	const auto update = [&](PhotoSize size, const ImageWithLocation &data) {
		Data::UpdateCloudFile(
			_images[PhotoSizeIndex(size)],
			data,
			owner().cache(),
			Data::kImageCacheTag,
			[=](Data::FileOrigin origin) { load(size, origin); },
			[=](QImage preloaded) {
				if (const auto media = activeMediaView()) {
					media->set(size, data.preloaded);
				}
			});
	};
	update(PhotoSize::Small, small);
	update(PhotoSize::Thumbnail, thumbnail);
	update(PhotoSize::Large, large);
}

int PhotoData::width() const {
	return _images[PhotoSizeIndex(PhotoSize::Large)].location.width();
}

int PhotoData::height() const {
	return _images[PhotoSizeIndex(PhotoSize::Large)].location.height();
}

PhotoClickHandler::PhotoClickHandler(
	not_null<PhotoData*> photo,
	FullMsgId context,
	PeerData *peer)
: FileClickHandler(context)
, _session(&photo->session())
, _photo(photo)
, _peer(peer) {
}

void PhotoOpenClickHandler::onClickImpl() const {
	if (valid()) {
		Core::App().showPhoto(this);
	}
}

void PhotoSaveClickHandler::onClickImpl() const {
	if (!valid()) {
		return;
	}
	const auto data = photo();
	if (!data->date) {
		return;
	} else {
		data->load(context());
	}
}

void PhotoCancelClickHandler::onClickImpl() const {
	if (!valid()) {
		return;
	}
	const auto data = photo();
	if (!data->date) {
		return;
	} else if (data->uploading()) {
		if (const auto item = data->owner().message(context())) {
			App::main()->cancelUploadLayer(item);
		}
	} else {
		data->cancel();
	}
}
