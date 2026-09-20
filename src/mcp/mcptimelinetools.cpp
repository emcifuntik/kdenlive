/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "mcptools.h"

#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/docundostack.hpp"
#include "timeline2/model/timelinefunctions.hpp"
#include "timeline2/model/timelineitemmodel.hpp"
#include "transitions/transitionsrepository.hpp"

#include <QJsonDocument>
#include <algorithm>

using namespace Mcp;

namespace {
QJsonArray ids(const std::unordered_set<int> &values)
{
    std::vector<int> ordered(values.begin(), values.end());
    std::sort(ordered.begin(), ordered.end());
    QJsonArray result;
    for (int id : ordered) {
        result.append(id);
    }
    return result;
}
std::unordered_set<int> idSet(const QJsonArray &values)
{
    std::unordered_set<int> result;
    for (const auto &value : values) {
        result.insert(value.toInt());
    }
    return result;
}
bool validItems(const std::shared_ptr<TimelineItemModel> &model, const std::unordered_set<int> &items)
{
    return std::all_of(items.begin(), items.end(), [&](int id) { return model->isItem(id) || model->isSubTitle(id); });
}
} // namespace

QJsonObject McpTools::trackInfo(int id)
{
    const auto model = timeline();
    return {{"track_id", id},
            {"position", model->getTrackPosition(id)},
            {"name", model->getTrackFullName(id)},
            {"audio", model->isAudioTrack(id)},
            {"locked", model->trackIsLocked(id)},
            {"hide", model->getTrackProperty(id, QStringLiteral("hide")).toInt()},
            {"active", model->getTrackProperty(id, QStringLiteral("kdenlive:timeline_active")).toInt() == 1},
            {"clips", model->getTrackClipsCount(id)},
            {"compositions", model->getTrackCompositionsCount(id)}};
}

QJsonObject McpTools::itemInfo(int id)
{
    const auto model = timeline();
    QJsonObject result{{"item_id", id},
                       {"track_id", model->getItemTrackId(id)},
                       {"position", model->getItemPosition(id)},
                       {"duration", model->getItemPlaytime(id)},
                       {"end_exclusive", model->getItemPosition(id) + model->getItemPlaytime(id)},
                       {"grouped", model->isInGroup(id)}};
    if (model->isClip(id)) {
        const auto source = model->getClipInOut(id);
        result.insert("kind", "clip");
        result.insert("bin_id", model->getClipBinId(id));
        result.insert("name", model->getClipName(id));
        result.insert("in", source.first);
        result.insert("out", source.second);
        result.insert("speed", model->getClipSpeed(id));
        result.insert("state", int(model->getClipState(id).first));
        result.insert("mix_duration", model->getMixDuration(id));
        result.insert("av_partner", model->getClipSplitPartner(id));
    } else if (model->isComposition(id)) {
        result.insert("kind", "composition");
    } else {
        result.insert("kind", "subtitle");
        result.insert("layer", model->getSubtitleLayer(id));
    }
    return result;
}

QJsonArray McpTools::timelineItems(int trackId)
{
    const auto model = timeline();
    QJsonArray items;
    for (int row = 0; row < model->rowCount(); ++row) {
        const auto index = model->index(row);
        if (trackId >= 0 && int(index.internalId()) != trackId) {
            continue;
        }
        for (int child = 0; child < model->rowCount(index); ++child) {
            const int id = int(model->index(child, 0, index).internalId());
            if (model->isClip(id) || model->isComposition(id)) {
                items.append(itemInfo(id));
            }
        }
    }
    return items;
}

void McpTools::registerTimelineTools()
{
    const auto frame = integer(0, 1000000000);
    const auto duration = integer(1, 100000000);
    const auto itemIds = array(integer(), 1, 1000);

    add("timeline_get", "Inspect tracks, clips and compositions in the active sequence. Ranges use an exclusive end; source clip out is inclusive.",
        {{"track_id", integer()}, {"offset", integer()}, {"limit", integer(1, 2000)}}, {}, true, [](const QJsonObject &a) {
            const auto model = timeline();
            if (a.contains("track_id") && !model->isTrack(a["track_id"].toInt())) {
                return failure("Unknown track.");
            }
            QJsonArray tracks;
            for (int pos = 0; pos < model->getTracksCount(); ++pos) {
                tracks.append(trackInfo(model->getTrackIndexFromPosition(pos)));
            }
            const auto all = timelineItems(a["track_id"].toInt(-1));
            QJsonArray items;
            const int offset = a["offset"].toInt(), limit = a["limit"].toInt(500);
            for (int i = offset; i < all.size() && items.size() < limit; ++i) {
                items.append(all[i]);
            }
            return success({{"sequence_uuid", model->uuid().toString()},
                            {"duration", model->duration()},
                            {"tracks", tracks},
                            {"items", items},
                            {"total_items", all.size()},
                            {"selection", ids(model->getCurrentSelection())},
                            {"groups", model->groupsData()}});
        });

    add("timeline_item_get", "Inspect one timeline clip, composition or subtitle by its timeline item ID.", {{"item_id", integer()}}, {"item_id"}, true,
        [](const QJsonObject &a) {
            const int id = a["item_id"].toInt();
            return timeline()->isItem(id) || timeline()->isSubTitle(id) ? success(itemInfo(id)) : failure("Unknown timeline item.");
        });

    add("track_create", "Create an audio or video track at a zero-based track position. Omit position to append. Returns a stable track ID.",
        {{"name", string()}, {"audio", boolean()}, {"position", integer(0, 128)}}, {"name", "audio"}, false, [](const QJsonObject &a) {
            const auto model = timeline();
            const int pos = a["position"].toInt(model->getTracksCount());
            if (pos > model->getTracksCount() || model->getTracksCount() >= 128) {
                return failure("Invalid position or track limit reached.");
            }
            int id = -1;
            if (!model->requestTrackInsertion(pos, id, a["name"].toString(), a["audio"].toBool())) {
                return failure("Could not create track.");
            }
            return success(trackInfo(id));
        });

    add("track_delete", "Delete a track and all its items. Undoable. At least one track must remain.", {{"track_id", integer()}}, {"track_id"}, false,
        [](const QJsonObject &a) {
            const auto model = timeline();
            const int id = a["track_id"].toInt();
            if (!model->isTrack(id) || model->getTracksCount() <= 1 || model->trackIsLocked(id)) {
                return failure("Unknown, locked or last track.");
            }
            return booleanResult(model->requestTrackDeletion(id));
        });

    add("track_rename", "Rename a timeline track. Undoable.", {{"track_id", integer()}, {"name", string()}}, {"track_id", "name"}, false,
        [](const QJsonObject &a) {
            if (!timeline()->isTrack(a["track_id"].toInt())) {
                return failure("Unknown track.");
            }
            timeline()->setTrackName(a["track_id"].toInt(), a["name"].toString());
            return success(trackInfo(a["track_id"].toInt()));
        });

    add("track_lock", "Set the track lock state. Undoable.", {{"track_id", integer()}, {"locked", boolean()}}, {"track_id", "locked"}, false,
        [](const QJsonObject &a) {
            const auto model = timeline();
            const int id = a["track_id"].toInt();
            if (!model->isTrack(id)) {
                return failure("Unknown track.");
            }
            if (model->trackIsLocked(id) != a["locked"].toBool()) {
                model->setTrackLockedState(id, a["locked"].toBool());
            }
            return success(trackInfo(id));
        });

    add("track_set_state", "Set track mute/visibility or editing-active state. hide: 0=none, 1=video, 2=audio, 3=both. Undoable.",
        {{"track_id", integer()}, {"hide", integer(0, 3)}, {"active", boolean()}}, {"track_id"}, false, [](const QJsonObject &a) {
            const auto model = timeline();
            const int id = a["track_id"].toInt();
            if (!model->isTrack(id)) {
                return failure("Unknown track.");
            }
            QMap<QString, QString> before, after;
            if (a.contains("hide")) {
                after.insert(QStringLiteral("hide"), QString::number(a["hide"].toInt()));
            }
            if (a.contains("active")) {
                after.insert(QStringLiteral("kdenlive:timeline_active"), a["active"].toBool() ? QStringLiteral("1") : QStringLiteral("0"));
            }
            if (after.isEmpty()) {
                return failure("Supply hide or active.");
            }
            for (auto it = after.cbegin(); it != after.cend(); ++it) {
                before.insert(it.key(), model->getTrackProperty(id, it.key()).toString());
            }
            Fun undo = [model, id, before]() {
                for (auto it = before.cbegin(); it != before.cend(); ++it) {
                    model->setTrackProperty(id, it.key(), it.value());
                }
                return true;
            };
            Fun redo = [model, id, after]() {
                for (auto it = after.cbegin(); it != after.cend(); ++it) {
                    model->setTrackProperty(id, it.key(), it.value());
                }
                return true;
            };
            redo();
            pCore->pushUndo(undo, redo, QStringLiteral("Set track state"));
            return success(trackInfo(id));
        });

    add("clip_insert",
        "Insert a bin clip into free timeline space. Source in/out are inclusive. stream=av inserts linked audio/video; default uses the destination track "
        "type. Undoable.",
        {{"bin_id", string()}, {"track_id", integer()}, {"position", frame}, {"in", frame}, {"out", frame}, {"stream", enumeration({"audio", "video", "av"})}},
        {"bin_id", "track_id", "position"}, false, [](const QJsonObject &a) {
            const auto model = timeline();
            const auto clip = pCore->projectItemModel()->getClipByBinID(a["bin_id"].toString());
            const int track = a["track_id"].toInt();
            if (!clip || !clip->statusReady() || !model->isTrack(track) || model->trackIsLocked(track)) {
                return failure("Expected a ready bin clip and unlocked track.");
            }
            const QString stream = a["stream"].toString(model->isAudioTrack(track) ? QStringLiteral("audio") : QStringLiteral("video"));
            if ((stream == QLatin1String("audio") && (!clip->hasAudio() || !model->isAudioTrack(track))) ||
                (stream == QLatin1String("video") && (!clip->hasVideo() || model->isAudioTrack(track)))) {
                return failure("Stream and destination track types do not match.");
            }
            const int in = a["in"].toInt(), out = a["out"].toInt(clip->getFramePlaytime() - 1);
            if (in > out || out >= clip->getFramePlaytime()) {
                return failure("Invalid source range.");
            }
            QString source = clip->clipId() + QStringLiteral("/%1/%2").arg(in).arg(out);
            if (stream != QLatin1String("av")) {
                source.prepend(stream == QLatin1String("audio") ? QLatin1Char('A') : QLatin1Char('V'));
            }
            if (!model->clipAudioStreamInfo(source, track, false).value(2).toBool()) {
                return failure("Create enough audio tracks before inserting this clip's streams.");
            }
            Fun undo = []() { return true; }, redo = undo;
            int id = -1;
            // Use the composable overload to avoid the interactive 'add audio tracks' prompt.
            if (!model->requestClipInsertion(source, track, a["position"].toInt(), id, true, true, false, undo, redo)) {
                undo();
                return failure("Insertion failed (collision, incompatible media, nesting or insufficient audio tracks).");
            }
            if (id < 0 || model->getClipPosition(id) != a["position"].toInt()) {
                undo();
                return failure("The clip cannot be inserted at the exact requested position.");
            }
            pCore->pushUndo(undo, redo, QStringLiteral("Insert clip"));
            return success(itemInfo(id));
        });

    add("timeline_insert_zone",
        "Insert or overwrite a source zone across explicit destination tracks. At most one video track; audio tracks receive active source streams in supplied "
        "order. Source end is exclusive. Insert mode shifts later clips. One undo command.",
        {{"bin_id", string()}, {"track_ids", itemIds}, {"position", frame}, {"in", frame}, {"end", frame}, {"overwrite", boolean()}},
        {"bin_id", "track_ids", "position", "in", "end"}, false, [](const QJsonObject &a) {
            const auto model = timeline();
            const auto clip = pCore->projectItemModel()->getClipByBinID(a["bin_id"].toString());
            const int in = a["in"].toInt(), end = a["end"].toInt();
            if (!clip || !clip->statusReady() || in >= end || end > clip->getFramePlaytime()) {
                return failure("Invalid ready clip or source zone.");
            }
            QList<int> tracks;
            for (const auto &entry : a["track_ids"].toArray()) {
                const int id = entry.toInt();
                if (!model->isTrack(id) || model->trackIsLocked(id) || tracks.contains(id)) {
                    return failure("Unknown, duplicate or locked destination track.");
                }
                tracks.append(id);
            }
            return booleanResult(
                TimelineFunctions::insertZoneOnTracks(model, tracks, clip->clipId(), a["position"].toInt(), QPoint(in, end), a["overwrite"].toBool()),
                "Zone insertion failed (incompatible tracks/streams, collision, nesting or a locked group); changes were rolled back.");
        });

    add("timeline_item_move", "Move a clip/composition to another track/position. Linked and grouped items move with it. Undoable.",
        {{"item_id", integer()}, {"track_id", integer()}, {"position", frame}}, {"item_id", "track_id", "position"}, false, [](const QJsonObject &a) {
            const auto model = timeline();
            const int id = a["item_id"].toInt(), track = a["track_id"].toInt();
            if ((!model->isClip(id) && !model->isComposition(id)) || !model->isTrack(track)) {
                return failure("Unknown item or track.");
            }
            const bool ok = model->isClip(id) ? model->requestClipMove(id, track, a["position"].toInt(), true, true, true, true)
                                              : model->requestCompositionMove(id, track, a["position"].toInt());
            return ok ? success(itemInfo(id)) : failure("Move rejected (collision, track lock or type mismatch).");
        });

    add("timeline_item_delete", "Delete a timeline item, respecting linked groups. Undoable.", {{"item_id", integer()}}, {"item_id"}, false,
        [](const QJsonObject &a) {
            const int id = a["item_id"].toInt();
            if (!timeline()->isItem(id) && !timeline()->isSubTitle(id)) {
                return failure("Unknown item.");
            }
            return booleanResult(timeline()->requestItemDeletion(id));
        });

    add("timeline_item_resize", "Trim/extend the left or right edge of a timeline item to a duration in frames. ripple=true shifts following clips. Undoable.",
        {{"item_id", integer()}, {"duration", duration}, {"right", boolean()}, {"ripple", boolean()}, {"single", boolean()}, {"move_guides", boolean()}},
        {"item_id", "duration", "right"}, false, [](const QJsonObject &a) {
            const auto model = timeline();
            const int id = a["item_id"].toInt();
            if (!model->isItem(id) && !model->isSubTitle(id)) {
                return failure("Unknown item.");
            }
            const int result = a["ripple"].toBool() ? model->requestItemRippleResize(model, id, a["duration"].toInt(), a["right"].toBool(), true,
                                                                                     a["move_guides"].toBool(), -1, a["single"].toBool())
                                                    : model->requestItemResize(id, a["duration"].toInt(), a["right"].toBool(), true, -1, a["single"].toBool());
            return result >= 0 ? success(itemInfo(id)) : failure("Resize rejected.");
        });

    add("clip_cut", "Split a timeline clip at an absolute timeline frame. Linked audio/video are split together. Returns the resulting timeline snapshot.",
        {{"item_id", integer()}, {"position", frame}}, {"item_id", "position"}, false, [](const QJsonObject &a) {
            const auto model = timeline();
            const int id = a["item_id"].toInt(), pos = a["position"].toInt();
            if (!model->isClip(id) || pos <= model->getItemPosition(id) || pos >= model->getItemPosition(id) + model->getItemPlaytime(id)) {
                return failure("Cut must be strictly inside a clip.");
            }
            return TimelineFunctions::requestClipCut(model, id, pos) ? success({{"items", timelineItems()}}) : failure("Cut rejected.");
        });

    add("timeline_cut_all", "Split clips at an absolute frame on all active unlocked tracks. Undoable.", {{"position", frame}}, {"position"}, false,
        [](const QJsonObject &a) { return booleanResult(TimelineFunctions::requestClipCutAll(timeline(), a["position"].toInt())); });

    add("clip_slip", "Slip a clip's source in/out by a signed frame offset without changing its timeline duration. Undoable.",
        {{"item_id", integer()}, {"offset", integer(-100000000, 100000000)}, {"single", boolean()}}, {"item_id", "offset"}, false, [](const QJsonObject &a) {
            const int id = a["item_id"].toInt();
            if (!timeline()->isClip(id)) {
                return failure("Unknown clip.");
            }
            const int applied = timeline()->requestClipSlip(id, a["offset"].toInt(), true, a["single"].toBool());
            return applied != a["offset"].toInt() ? failure("Slip rejected.") : success({{"applied_offset", applied}, {"item", itemInfo(id)}});
        });

    add("clip_duplicate", "Copy a timeline clip or composition, including effects, to another track and position. Undoable.",
        {{"item_id", integer()}, {"track_id", integer()}, {"position", frame}}, {"item_id", "track_id", "position"}, false, [](const QJsonObject &a) {
            const auto model = timeline();
            const int id = a["item_id"].toInt(), track = a["track_id"].toInt();
            if ((!model->isClip(id) && !model->isComposition(id)) || !model->isTrack(track)) {
                return failure("Unknown item or track.");
            }
            return TimelineFunctions::requestItemCopy(model, id, track, a["position"].toInt()) ? success({{"items", timelineItems(track)}})
                                                                                               : failure("Copy rejected.");
        });

    add("clip_speed",
        "Change constant playback speed as a multiplier: 1=normal, 2=double, -1=reverse. pitch_compensate preserves audio pitch; change_duration defaults "
        "true.",
        {{"item_id", integer()}, {"speed", number(-100., 100.)}, {"pitch_compensate", boolean()}, {"change_duration", boolean()}}, {"item_id", "speed"}, false,
        [](const QJsonObject &a) {
            const int id = a["item_id"].toInt();
            const double speed = a["speed"].toDouble();
            if (!timeline()->isClip(id) || qAbs(speed) < 0.01) {
                return failure("Unknown clip or speed magnitude below 0.01.");
            }
            return booleanResult(timeline()->requestClipTimeWarp(id, speed * 100., a["pitch_compensate"].toBool(true), a["change_duration"].toBool(true)));
        });

    add("clip_set_state", "Enable a clip's audio/video stream or disable it. Track compatibility still applies. Undoable.",
        {{"item_id", integer()}, {"state", enumeration({"audio", "video", "disabled"})}}, {"item_id", "state"}, false, [](const QJsonObject &a) {
            const auto model = timeline();
            const int id = a["item_id"].toInt();
            if (!model->isClip(id)) {
                return failure("Unknown clip.");
            }
            const QMap<QString, PlaylistState::ClipState> states{
                {"audio", PlaylistState::AudioOnly}, {"video", PlaylistState::VideoOnly}, {"disabled", PlaylistState::Disabled}};
            if ((a["state"] == QLatin1String("audio") && !model->isAudioTrack(model->getItemTrackId(id))) ||
                (a["state"] == QLatin1String("video") && model->isAudioTrack(model->getItemTrackId(id)))) {
                return failure("The stream must match the track type.");
            }
            Fun undo = []() { return true; }, redo = undo;
            if (!TimelineFunctions::changeClipState(model, id, states.value(a["state"].toString()), undo, redo)) {
                undo();
                return failure("Incompatible clip state.");
            }
            pCore->pushUndo(undo, redo, QStringLiteral("Set clip state"));
            return success(itemInfo(id));
        });

    add("clip_split_audio", "Restore/split a clip's audio onto explicit audio tracks and link it to the video. Undoable.",
        {{"item_id", integer()}, {"track_ids", itemIds}}, {"item_id", "track_ids"}, false, [](const QJsonObject &a) {
            const auto model = timeline();
            if (!model->isClip(a["item_id"].toInt())) {
                return failure("Unknown clip.");
            }
            QList<int> tracks;
            for (const auto &value : a["track_ids"].toArray()) {
                const int id = value.toInt();
                if (!model->isTrack(id) || !model->isAudioTrack(id) || tracks.contains(id)) {
                    return failure("Expected unique audio track IDs.");
                }
                tracks.append(id);
            }
            return booleanResult(TimelineFunctions::requestSplitAudio(model, a["item_id"].toInt(), tracks));
        });

    add("clip_split_video", "Restore/split a clip's video onto an explicit video track. Undoable.", {{"item_id", integer()}, {"track_id", integer()}},
        {"item_id", "track_id"}, false, [](const QJsonObject &a) {
            const auto model = timeline();
            const int track = a["track_id"].toInt();
            if (!model->isClip(a["item_id"].toInt()) || !model->isTrack(track) || model->isAudioTrack(track)) {
                return failure("Expected a clip and video track.");
            }
            return booleanResult(TimelineFunctions::requestSplitVideo(model, a["item_id"].toInt(), track));
        });

    add("timeline_select", "Replace the timeline selection. An empty item_ids array clears selection.", {{"item_ids", array(integer(), 0, 1000)}}, {"item_ids"},
        false, [](const QJsonObject &a) {
            const auto selection = idSet(a["item_ids"].toArray());
            if (!validItems(timeline(), selection)) {
                return failure("Unknown selection item.");
            }
            return booleanResult(timeline()->requestSetSelection(selection));
        });

    add("group_create", "Group timeline items so they move together. Undoable.", {{"item_ids", array(integer(), 2, 1000)}}, {"item_ids"}, false,
        [](const QJsonObject &a) {
            const auto selection = idSet(a["item_ids"].toArray());
            if (selection.size() < 2 || !validItems(timeline(), selection)) {
                return failure("Expected at least two distinct valid items.");
            }
            const int id = timeline()->requestClipsGroup(selection);
            return id < 0 ? failure("Grouping failed.") : success({{"group_id", id}});
        });

    add("group_remove", "Ungroup the group containing an item. Undoable.", {{"item_id", integer()}}, {"item_id"}, false, [](const QJsonObject &a) {
        const int id = a["item_id"].toInt();
        if (!timeline()->isItem(id) || !timeline()->isInGroup(id)) {
            return failure("Expected a grouped item.");
        }
        return booleanResult(timeline()->requestClipUngroup(id));
    });

    add("group_get", "List the leaf items in the group containing an item.", {{"item_id", integer()}}, {"item_id"}, true, [](const QJsonObject &a) {
        const int id = a["item_id"].toInt();
        if (!timeline()->isItem(id)) {
            return failure("Unknown item.");
        }
        return success({{"item_ids", ids(timeline()->getGroupElements(id))}});
    });

    for (const bool extract : {true, false}) {
        add(extract ? "timeline_extract" : "timeline_lift",
            extract ? "Remove a timeline zone and close the gap on supplied tracks. End is exclusive. Undoable."
                    : "Remove a timeline zone leaving a gap on supplied tracks. End is exclusive. Undoable.",
            {{"track_ids", itemIds}, {"start", frame}, {"end", frame}}, {"track_ids", "start", "end"}, false, [extract](const QJsonObject &a) {
                const auto model = timeline();
                if (a["start"].toInt() >= a["end"].toInt()) {
                    return failure("Zone end must exceed start.");
                }
                QVector<int> tracks;
                for (const auto &value : a["track_ids"].toArray()) {
                    if (!model->isTrack(value.toInt()) || model->trackIsLocked(value.toInt()) || tracks.contains(value.toInt())) {
                        return failure("Unknown, duplicate or locked track.");
                    }
                    tracks.append(value.toInt());
                }
                return booleanResult(TimelineFunctions::extractZone(model, tracks, QPoint(a["start"].toInt(), a["end"].toInt()), !extract));
            });
    }

    add("timeline_insert_space", "Insert blank space on explicit tracks, moving later clips. start/end define the new gap with exclusive end. Undoable.",
        {{"track_ids", itemIds}, {"start", frame}, {"end", frame}}, {"track_ids", "start", "end"}, false, [](const QJsonObject &a) {
            const auto model = timeline();
            if (a["start"].toInt() >= a["end"].toInt()) {
                return failure("Zone end must exceed start.");
            }
            QVector<int> tracks;
            for (const auto &value : a["track_ids"].toArray()) {
                if (!model->isTrack(value.toInt()) || model->trackIsLocked(value.toInt()) || tracks.contains(value.toInt())) {
                    return failure("Unknown, duplicate or locked track.");
                }
                tracks.append(value.toInt());
            }
            Fun undo = []() { return true; }, redo = undo;
            if (!TimelineFunctions::requestInsertSpace(model, QPoint(a["start"].toInt(), a["end"].toInt()), undo, redo, tracks)) {
                undo();
                return failure("Could not insert space.");
            }
            pCore->pushUndo(undo, redo, QStringLiteral("Insert space"));
            return success();
        });

    for (const bool all : {true, false}) {
        add(all ? "timeline_remove_gaps" : "timeline_remove_gap",
            all ? "Remove all gaps from a position on one track. Undoable." : "Close the gap at a track position. Undoable.",
            {{"track_id", integer()}, {"position", frame}}, {"track_id", "position"}, false, [all](const QJsonObject &a) {
                const auto model = timeline();
                const int track = a["track_id"].toInt(), pos = a["position"].toInt();
                if (!model->isTrack(track) || model->trackIsLocked(track)) {
                    return failure("Unknown or locked track.");
                }
                return booleanResult(all ? TimelineFunctions::requestDeleteAllBlanksFrom(model, track, pos)
                                         : TimelineFunctions::requestDeleteBlankAt(model, track, pos, false));
            });
    }

    add("composition_create", "Add an installed composition/transition between video tracks. Discover IDs with assets_list kind=transition. Undoable.",
        {{"asset_id", string()}, {"track_id", integer()}, {"position", frame}, {"duration", duration}}, {"asset_id", "track_id", "position", "duration"}, false,
        [](const QJsonObject &a) {
            const auto model = timeline();
            const int track = a["track_id"].toInt();
            if (!model->isTrack(track) || model->isAudioTrack(track) || !TransitionsRepository::get()->exists(a["asset_id"].toString())) {
                return failure("Unknown video track or transition.");
            }
            int id = -1;
            if (!model->requestCompositionInsertion(a["asset_id"].toString(), track, a["position"].toInt(), a["duration"].toInt(), nullptr, id)) {
                return failure("Composition insertion failed.");
            }
            return success(itemInfo(id));
        });

    add("composition_set_track", "Choose the composition's lower source track; omit source_track_id for automatic selection. Undoable.",
        {{"item_id", integer()}, {"source_track_id", integer()}}, {"item_id"}, false, [](const QJsonObject &a) {
            const auto model = timeline();
            const int id = a["item_id"].toInt();
            if (!model->isComposition(id)) {
                return failure("Unknown composition.");
            }
            int track = -1;
            if (a.contains("source_track_id")) {
                const int source = a["source_track_id"].toInt();
                if (!model->isTrack(source) || model->isAudioTrack(source) ||
                    model->getTrackPosition(source) >= model->getTrackPosition(model->getItemTrackId(id))) {
                    return failure("The source must be a lower video track.");
                }
                track = model->getTrackMltIndex(source);
            }
            TimelineFunctions::setCompositionATrack(model, id, track);
            return success();
        });

    add("mix_create", "Create a same-track mix/crossfade at a clip boundary using an installed transition. Undoable.",
        {{"item_id", integer()}, {"asset_id", string()}}, {"item_id"}, false, [](const QJsonObject &a) {
            const int id = a["item_id"].toInt();
            const QString assetId = a["asset_id"].toString(QStringLiteral("luma"));
            if (!timeline()->isClip(id) || !TransitionsRepository::get()->exists(assetId)) {
                return failure("Unknown clip or transition.");
            }
            return booleanResult(timeline()->mixClip(id, assetId));
        });

    add("mix_resize", "Set the duration/alignment of a clip's incoming same-track mix. Undoable.",
        {{"item_id", integer()}, {"duration", duration}, {"alignment", enumeration({"left", "center", "right"})}}, {"item_id", "duration", "alignment"}, false,
        [](const QJsonObject &a) {
            const int id = a["item_id"].toInt();
            if (!timeline()->isClip(id) || timeline()->getMixDuration(id) <= 0) {
                return failure("The clip has no incoming mix.");
            }
            const QMap<QString, MixAlignment> alignments{
                {"left", MixAlignment::AlignLeft}, {"center", MixAlignment::AlignCenter}, {"right", MixAlignment::AlignRight}};
            timeline()->requestResizeMix(id, a["duration"].toInt(), alignments.value(a["alignment"].toString()));
            return success(itemInfo(id));
        });

    add("clip_time_remap_get", "Inspect native time-remap data. time_map keys are producer clock times and values are source seconds.",
        {{"item_id", integer()}}, {"item_id"}, true, [](const QJsonObject &a) {
            if (!timeline()->isClip(a["item_id"].toInt())) {
                return failure("Unknown clip.");
            }
            const auto values = timeline()->getClipTimeRemapValues(a["item_id"].toInt());
            QJsonObject result{{"enabled", !values.isEmpty()}};
            for (auto it = values.cbegin(); it != values.cend(); ++it) {
                result.insert(it.key(), it.value());
            }
            return success(result);
        });

    add("clip_time_remap_set",
        "Apply a variable-speed, reverse or freeze time map to a media clip and its linked AV partner. Keys map output frames relative to clip start to "
        "absolute source frames; include output 0 and duration-1. Linear segments; equal source frames freeze. Duration stays fixed. Undoable.",
        {{"item_id", integer()},
         {"keyframes", array(object({{"output", frame}, {"source", frame}}, {"output", "source"}), 2, 10000)},
         {"pitch_compensate", boolean()},
         {"frame_blend", boolean()}},
        {"item_id", "keyframes"}, false, [](const QJsonObject &a) {
            QMap<int, int> keys;
            for (const auto &entry : a["keyframes"].toArray()) {
                const auto key = entry.toObject();
                if (keys.contains(key["output"].toInt())) {
                    return failure("Duplicate output frame.");
                }
                keys.insert(key["output"].toInt(), key["source"].toInt());
            }
            return booleanResult(
                timeline()->requestClipTimeRemapKeyframes(a["item_id"].toInt(), keys, a["pitch_compensate"].toBool(true), a["frame_blend"].toBool()),
                "Time remap requires unlocked media clips at normal constant speed, valid source frames, and keys spanning the entire duration.");
        });

    add("clip_time_remap_clear", "Remove time remapping from a clip and its AV partner, restoring source timing. Undoable; duration may change.",
        {{"item_id", integer()}}, {"item_id"}, false, [](const QJsonObject &a) {
            const auto model = timeline();
            const int id = a["item_id"].toInt();
            if (!model->isClip(id)) {
                return failure("Unknown clip.");
            }
            QList<int> clips{id};
            if (model->getClipSplitPartner(id) >= 0) {
                clips.append(model->getClipSplitPartner(id));
            }
            for (int clip : clips) {
                if (model->getClipTrackId(clip) < 0 || model->trackIsLocked(model->getClipTrackId(clip))) {
                    return failure("Clip or AV partner is locked.");
                }
            }
            Fun undo = []() { return true; }, redo = undo;
            bool changed = false;
            for (int clip : clips) {
                if (model->getClipTimeRemapValues(clip).isEmpty()) {
                    continue;
                }
                if (!model->requestClipTimeRemap(clip, false, undo, redo)) {
                    undo();
                    return failure("Could not remove time remap.");
                }
                changed = true;
            }
            if (changed) {
                pCore->pushUndo(undo, redo, QStringLiteral("Remove time remap"));
            }
            return success(itemInfo(id));
        });

    add("timeline_copy", "Serialize selected timeline items, groups and effects in native Kdenlive scene XML for inspection/archival.", {{"item_ids", itemIds}},
        {"item_ids"}, true, [](const QJsonObject &a) {
            const auto selection = idSet(a["item_ids"].toArray());
            if (!validItems(timeline(), selection)) {
                return failure("Unknown item.");
            }
            const QString xml = TimelineFunctions::copyClips(timeline(), selection);
            return xml.isEmpty() ? failure("Could not serialize selection.") : success({{"xml", xml}});
        });

    add("timeline_duplicate_items",
        "Duplicate multiple items, preserving relative timing, tracks, groups and effects. track_id anchors the destination; position is the new start. "
        "Undoable.",
        {{"item_ids", itemIds}, {"track_id", integer()}, {"position", frame}}, {"item_ids", "track_id", "position"}, false, [](const QJsonObject &a) {
            const auto model = timeline();
            const auto selection = idSet(a["item_ids"].toArray());
            const int track = a["track_id"].toInt();
            if (!validItems(model, selection) || !model->isTrack(track) || model->trackIsLocked(track)) {
                return failure("Invalid items or destination track.");
            }
            const QString xml = TimelineFunctions::copyClips(model, selection);
            if (xml.isEmpty()) {
                return failure("Could not serialize selection.");
            }
            return TimelineFunctions::pasteClips(model, xml, track, a["position"].toInt()) ? success({{"items", timelineItems()}})
                                                                                           : failure("Duplicate rejected.");
        });

    add("mix_remove", "Remove a clip's incoming same-track mix. Undoable.", {{"item_id", integer()}}, {"item_id"}, false, [](const QJsonObject &a) {
        const int id = a["item_id"].toInt();
        if (!timeline()->isClip(id) || timeline()->getMixDuration(id) <= 0) {
            return failure("The clip has no incoming mix.");
        }
        return booleanResult(timeline()->removeMix(id));
    });
}
