/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "timelinemodel.hpp"

#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "clipmodel.hpp"
#include "core.h"
#include "undohelper.hpp"

#include <KLocalizedString>
#include <cstdlib>

QMap<QString, QString> TimelineModel::getClipTimeRemapValues(int clipId) const
{
    QReadLocker locker(&m_lock);
    const auto it = m_allClips.find(clipId);
    return it != m_allClips.end() && it->second->hasTimeRemap() ? it->second->getRemapValues() : QMap<QString, QString>();
}

bool TimelineModel::requestClipTimeRemapKeyframes(int clipId, const QMap<int, int> &keyframes, bool pitchCompensate, bool frameBlend)
{
    QWriteLocker locker(&m_lock);
    if (!isClip(clipId) || keyframes.size() < 2 || keyframes.size() > 10000 || keyframes.firstKey() != 0 ||
        keyframes.lastKey() != getClipPlaytime(clipId) - 1) {
        return false;
    }
    QList<int> clips{clipId};
    const int partner = getClipSplitPartner(clipId);
    if (partner >= 0) {
        clips.append(partner);
    }
    for (int id : clips) {
        if (!isClip(id) || getClipTrackId(id) < 0 || trackIsLocked(getClipTrackId(id)) || !qFuzzyCompare(getClipSpeed(id), 1.) ||
            getClipPlaytime(id) != getClipPlaytime(clipId)) {
            return false;
        }
        const auto source = pCore->projectItemModel()->getClipByBinID(getClipBinId(id));
        if (!source || !source->statusReady() ||
            (source->clipType() != ClipType::AV && source->clipType() != ClipType::Audio && source->clipType() != ClipType::Video)) {
            return false;
        }
        for (int sourceFrame : keyframes) {
            if (sourceFrame < 0 || sourceFrame >= source->getFramePlaytime()) {
                return false;
            }
        }
    }
    Fun undo = []() { return true; }, redo = undo;
    for (int id : clips) {
        const auto clip = m_allClips.at(id);
        if (!clip->hasTimeRemap() && !requestClipTimeRemap(id, true, undo, redo)) {
            undo();
            return false;
        }
        if (!clip->hasTimeRemap()) {
            undo();
            return false;
        }
        Mlt::Properties properties;
        properties.set("_profile", pCore->getProjectProfile().get_profile(), 0);
        for (auto it = keyframes.cbegin(); it != keyframes.cend(); ++it) {
            // Match the editor's endpoint convention: the final map key follows the last frame.
            const int output = clip->getIn() + it.key() + (it.key() == keyframes.lastKey() ? 1 : 0);
            properties.anim_set("map", GenTime(it.value(), pCore->getCurrentFps()).seconds(), output, 0, mlt_keyframe_linear);
        }
        Mlt::Animation animation(properties.get_animation("map"));
        char *serialized = animation.serialize_cut(mlt_time_clock);
        const QString map = QString::fromUtf8(serialized);
        free(serialized);
        const auto before = clip->getRemapValues();
        const QMap<QString, QString> after{{QStringLiteral("time_map"), map},
                                           {QStringLiteral("pitch"), pitchCompensate && clipIsAudio(id) ? QStringLiteral("1") : QStringLiteral("0")},
                                           {QStringLiteral("image_mode"), frameBlend ? QStringLiteral("blend") : QStringLiteral("nearest")}};
        const std::weak_ptr<TimelineModel> weak = shared_from_this();
        const auto apply = [weak, clip, id](const QMap<QString, QString> &values) {
            auto model = weak.lock();
            if (!model) {
                return false;
            }
            for (auto it = values.cbegin(); it != values.cend(); ++it) {
                clip->setRemapValue(it.key(), it.value());
            }
            model->requestClipUpdate(id, {TimeRemapRole, ResourceRole, ReloadAudioThumbRole});
            model->checkRefresh(clip->getPosition(), clip->getPosition() + clip->getPlaytime());
            return true;
        };
        Fun reverse = [apply, before]() { return apply(before); };
        Fun forward = [apply, after]() { return apply(after); };
        if (!forward()) {
            undo();
            return false;
        }
        UPDATE_UNDO_REDO(forward, reverse, undo, redo);
    }
    pCore->pushUndo(undo, redo, i18n("Edit time remap"));
    Q_EMIT refreshClipActions();
    return true;
}
