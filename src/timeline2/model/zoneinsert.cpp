/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "timelinefunctions.hpp"

#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "timelineitemmodel.hpp"

#include <KLocalizedString>

bool TimelineFunctions::insertZoneOnTracks(const std::shared_ptr<TimelineItemModel> &model, const QList<int> &tracks, const QString &binId, int position,
                                           QPoint sourceZone, bool overwrite)
{
    const auto clip = pCore->projectItemModel()->getClipByBinID(binId);
    if (!clip || !clip->statusReady() || position < 0 || tracks.isEmpty() || sourceZone.x() < 0 || sourceZone.x() >= sourceZone.y() ||
        sourceZone.y() > clip->getFramePlaytime() || !clip->canBeDropped(model->uuid())) {
        return false;
    }
    int videoTracks = 0, audioTracks = 0;
    const auto streams = clip->activeStreams().keys();
    QSet<int> unique;
    for (int track : tracks) {
        if (!model->isTrack(track) || model->trackIsLocked(track) || unique.contains(track)) {
            return false;
        }
        unique.insert(track);
        if (model->isAudioTrack(track)) {
            ++audioTracks;
        } else {
            ++videoTracks;
        }
    }
    if (videoTracks > 1 || (videoTracks && !clip->hasVideo()) || audioTracks > streams.size()) {
        return false;
    }
    Fun undo = []() { return true; }, redo = undo;
    const QPoint zone(position, position + sourceZone.y() - sourceZone.x());
    bool ok = breakAffectedGroups(model, tracks, zone, undo, redo);
    if (overwrite) {
        for (int track : tracks) {
            ok = ok && liftZone(model, track, zone, undo, redo);
        }
    } else {
        for (int track : tracks) {
            const int id = model->getClipByPosition(track, position);
            if (id >= 0 && model->getClipPosition(id) < position) {
                ok = ok && requestClipCut(model, id, position, undo, redo);
            }
        }
        ok = ok && requestInsertSpace(model, zone, undo, redo, tracks);
    }
    std::unordered_set<int> inserted;
    int audioIndex = 0;
    const QString source = binId + QStringLiteral("/%1/%2").arg(sourceZone.x()).arg(sourceZone.y() - 1);
    for (int track : tracks) {
        if (!ok) {
            break;
        }
        const bool audio = model->isAudioTrack(track);
        int id = -1;
        ok = model->requestClipCreation(source, id, audio ? PlaylistState::AudioOnly : PlaylistState::VideoOnly, audio ? streams[audioIndex++] : -1, 1., false,
                                        undo, redo);
        ok = ok && model->requestClipMove(id, track, position, true, true, true, true, undo, redo) == TimelineModel::MoveSuccess;
        if (ok) {
            inserted.insert(id);
        }
    }
    if (ok && inserted.size() > 1) {
        ok = model->requestClipsGroup(inserted, undo, redo, videoTracks ? GroupType::AVSplit : GroupType::Normal) >= 0;
    }
    if (!ok) {
        undo();
        return false;
    }
    pCore->pushUndo(undo, redo, overwrite ? i18n("Overwrite zone") : i18n("Insert zone"));
    return true;
}
