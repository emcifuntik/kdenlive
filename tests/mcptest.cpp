/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "doc/kdenlivedoc.h"
#include "effects/effectsrepository.hpp"
#include "mcp/mcptools.h"
#include "test_utils.hpp"

#include <QJsonDocument>

TEST_CASE("MCP editor operations preserve model state and undo", "[MCP]")
{
    auto bin = pCore->projectItemModel();
    bin->clean();
    auto undo = std::make_shared<DocUndoStack>(nullptr);
    KdenliveDoc document(undo);
    pCore->projectManager()->testSetDocument(&document);
    QDateTime date = QDateTime::currentDateTime();
    KdenliveTests::updateTimeline(false, QString(), QString(), date, 0);
    auto timeline = document.getTimeline(document.uuid());
    pCore->projectManager()->testSetActiveTimeline(timeline);
    struct Cleanup
    {
        ~Cleanup() { pCore->projectManager()->closeCurrentDocument(false, false); }
    } cleanup;
    McpTools tools;
    // The model test runner has no MainWindow. Exercise the real adapters/models
    // while the transport tests independently cover guarding and input validation.
    tools.registry().setGuard({});
    auto call = [&](const QString &name, const QJsonObject &args = QJsonObject()) {
        const auto response = tools.registry().call(name, args);
        INFO(QString::fromUtf8(QJsonDocument(response).toJson()).toStdString());
        REQUIRE_FALSE(response["isError"].toBool());
        return response["structuredContent"].toObject();
    };
    const int track = call("track_create", {{"name", "MCP video"}, {"audio", false}})["track_id"].toInt();
    const auto source = KdenliveTests::createProducer(pCore->getProjectProfile(), "red", bin, 100);
    const int clip = call("clip_insert", {{"bin_id", source}, {"track_id", track}, {"position", 10}, {"in", 5}, {"out", 24}})["item_id"].toInt();
    REQUIRE(timeline->getClipPosition(clip) == 10);
    REQUIRE(timeline->getClipPlaytime(clip) == 20);
    REQUIRE(timeline->getClipIn(clip) == 5);

    SECTION("Collision and invalid arguments leave project unchanged")
    {
        const int before = undo->index();
        REQUIRE(tools.registry().call("clip_insert", {{"bin_id", source}, {"track_id", track}, {"position", 15}})["isError"].toBool());
        REQUIRE(tools.registry().call("timeline_item_resize", {{"item_id", clip}, {"duration", -1}, {"right", true}})["isError"].toBool());
        REQUIRE(tools.registry().call("timeline_item_move", {{"item_id", 2000000000}, {"track_id", track}, {"position", 0}})["isError"].toBool());
        REQUIRE(undo->index() == before);
        REQUIRE(timeline->getTrackClipsCount(track) == 1);
        REQUIRE(timeline->getClipPosition(clip) == 10);
    }
    SECTION("Trim, cut, undo and redo preserve source bounds")
    {
        call("timeline_item_resize", {{"item_id", clip}, {"duration", 12}, {"right", true}});
        REQUIRE(timeline->getClipPlaytime(clip) == 12);
        call("history_undo");
        REQUIRE(timeline->getClipPlaytime(clip) == 20);
        call("history_redo");
        REQUIRE(timeline->getClipPlaytime(clip) == 12);
        call("clip_cut", {{"item_id", clip}, {"position", 16}});
        REQUIRE(timeline->getTrackClipsCount(track) == 2);
        REQUIRE(timeline->getClipPlaytime(clip) == 6);
        call("history_undo");
        REQUIRE(timeline->getTrackClipsCount(track) == 1);
        REQUIRE(timeline->getClipPlaytime(clip) == 12);
    }
    SECTION("Speed is a multiplier and remains undoable")
    {
        KdenliveTests::makeFiniteClipEnd(timeline, clip);
        call("clip_speed", {{"item_id", clip}, {"speed", 2.0}, {"pitch_compensate", false}});
        REQUIRE(timeline->getClipSpeed(clip) == Approx(2.0));
        call("history_undo");
        REQUIRE(timeline->getClipSpeed(clip) == Approx(1.0));
    }
    SECTION("Explicit zone insertion shifts clips and overwrites without dialogs")
    {
        call("timeline_insert_zone", {{"bin_id", source}, {"track_ids", QJsonArray{track}}, {"position", 0}, {"in", 0}, {"end", 10}});
        REQUIRE(timeline->getClipPosition(clip) == 20);
        REQUIRE(timeline->getTrackClipsCount(track) == 2);
        call("history_undo");
        REQUIRE(timeline->getClipPosition(clip) == 10);
        REQUIRE(timeline->getTrackClipsCount(track) == 1);
        call("timeline_insert_zone", {{"bin_id", source}, {"track_ids", QJsonArray{track}}, {"position", 15}, {"in", 0}, {"end", 5}, {"overwrite", true}});
        REQUIRE(timeline->getTrackClipsCount(track) == 3);
        call("history_undo");
        REQUIRE(timeline->getTrackClipsCount(track) == 1);
        REQUIRE(timeline->getClipPlaytime(clip) == 20);
    }
    SECTION("Snapshot exposes real timeline IDs and locking prevents insertion")
    {
        const auto snapshot = call("timeline_get", {{"track_id", track}});
        REQUIRE(snapshot["items"].toArray().size() == 1);
        REQUIRE(snapshot["items"].toArray()[0].toObject()["item_id"].toInt() == clip);
        call("track_lock", {{"track_id", track}, {"locked", true}});
        REQUIRE(tools.registry().call("clip_insert", {{"bin_id", source}, {"track_id", track}, {"position", 100}})["isError"].toBool());
        call("history_undo");
        REQUIRE_FALSE(timeline->trackIsLocked(track));
    }
    SECTION("Full catalog and invalid time remap are inspectable without mutation")
    {
        REQUIRE(tools.registry().tools().size() >= 100);
        REQUIRE(call("assets_list", {{"kind", "effect"}, {"limit", 1}})["total"].toInt() > 0);
        const int before = undo->index();
        REQUIRE(tools.registry()
                    .call("clip_time_remap_set",
                          {{"item_id", clip},
                           {"keyframes", QJsonArray{QJsonObject{{"output", 0}, {"source", 0}}, QJsonObject{{"output", 999}, {"source", 10}}}}})["isError"]
                    .toBool());
        REQUIRE(undo->index() == before);
    }
    SECTION("Effect reordering uses final rows and initial keyframes stay protected")
    {
        EffectsRepository::get()->reloadCustom(sourcesPath + QStringLiteral("/../data/effects/brightness.xml"));
        const QJsonObject target{{"kind", "clip"}, {"item_id", clip}};
        const int first = call("effect_add", {{"target", target}, {"asset_id", "brightness"}})["effect_row"].toInt();
        const int second = call("effect_add", {{"target", target}, {"asset_id", "brightness"}})["effect_row"].toInt();
        const auto stack = timeline->getClipEffectStackModel(clip);
        const auto effect = stack->getEffectStackRow(first);
        call("effect_move", {{"target", target}, {"effect_row", first}, {"destination_row", second}});
        REQUIRE(stack->getEffectStackRow(second) == effect);
        call("history_undo");
        REQUIRE(stack->getEffectStackRow(first) == effect);
        auto args = QJsonObject{{"target", target}, {"effect_row", first}};
        const auto frames = call("keyframes_get", args)["keyframes"].toArray();
        REQUIRE_FALSE(frames.isEmpty());
        const int initial = frames[0].toObject()["frame"].toInt();
        args.insert("frame", initial);
        const int before = undo->index();
        REQUIRE(tools.registry().call("keyframe_remove", args)["isError"].toBool());
        args.insert("new_frame", 12);
        REQUIRE(tools.registry().call("keyframe_move", args)["isError"].toBool());
        REQUIRE(undo->index() == before);
    }
    REQUIRE(timeline->checkConsistency());
}
