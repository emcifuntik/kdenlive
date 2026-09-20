/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "mcptools.h"

#include <algorithm>

#include "assets/keyframes/model/keyframemodellist.hpp"
#include "assets/model/assetcommand.hpp"
#include "assets/model/assetparametermodel.hpp"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "effects/effectsrepository.hpp"
#include "effects/effectstack/model/effectitemmodel.hpp"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "timeline2/model/timelineitemmodel.hpp"
#include "transitions/transitionsrepository.hpp"

#include <QMetaEnum>

using namespace Mcp;

namespace {
std::shared_ptr<AbstractEffectItem> stackItem(const std::shared_ptr<EffectStackModel> &stack, const QJsonObject &args)
{
    if (!stack || (args.contains("effect_row") && args.contains("effect_path"))) {
        return nullptr;
    }
    QJsonArray path = args["effect_path"].toArray();
    if (path.isEmpty()) {
        if (!args.contains("effect_row")) {
            return nullptr;
        }
        path.append(args["effect_row"]);
    }
    std::shared_ptr<TreeItem> parent;
    std::shared_ptr<AbstractEffectItem> result;
    for (const auto &value : path) {
        const int row = value.toInt();
        const int count = parent ? parent->childCount() : stack->rowCount();
        if (row < 0 || row >= count) {
            return nullptr;
        }
        result = stack->getEffectStackRow(row, parent);
        if (!result) {
            return nullptr;
        }
        parent = result;
    }
    return result;
}
QJsonObject with(QJsonObject properties, const QJsonObject &extra)
{
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        properties.insert(it.key(), it.value());
    }
    return properties;
}
QString keyframeRangeError(const std::shared_ptr<AssetParameterModel> &model, int frame)
{
    if (model->rowCount() == 0) {
        return QStringLiteral("Asset has no parameters.");
    }
    const auto index = model->index(0, 0);
    const int in = model->data(index, AssetParameterModel::ParentInRole).toInt();
    const int duration = model->data(index, AssetParameterModel::ParentDurationRole).toInt();
    return frame < in || frame >= in + duration ? QStringLiteral("Keyframe frame must be within the asset's source in/duration range.") : QString();
}
} // namespace

QJsonObject McpTools::targetSchema()
{
    return object({{"kind", enumeration({"clip", "track", "master", "bin", "composition", "mix"})}, {"item_id", integer()}, {"bin_id", string()}}, {"kind"});
}

std::shared_ptr<EffectStackModel> McpTools::effectStack(const QJsonObject &args)
{
    const auto target = args["target"].toObject();
    const auto kind = target["kind"].toString();
    const int id = target["item_id"].toInt(-1);
    const auto model = timeline();
    if (kind == QLatin1String("master")) {
        return model->getMasterEffectStackModel();
    }
    if (kind == QLatin1String("clip") && model->isClip(id)) {
        return model->getClipEffectStackModel(id);
    }
    if (kind == QLatin1String("track") && model->isTrack(id)) {
        return model->getTrackEffectStackModel(id);
    }
    if (kind == QLatin1String("bin")) {
        const auto clip = pCore->projectItemModel()->getClipByBinID(target["bin_id"].toString());
        if (clip && clip->statusReady()) {
            return clip->getEffectStack();
        }
    }
    return nullptr;
}

std::shared_ptr<AssetParameterModel> McpTools::asset(const QJsonObject &args)
{
    const auto target = args["target"].toObject();
    if (target["kind"] == QLatin1String("mix")) {
        return timeline()->getMixParameterModel(target["item_id"].toInt(-1));
    }
    if (target["kind"] == QLatin1String("composition")) {
        const int id = target["item_id"].toInt(-1);
        return timeline()->isComposition(id) ? timeline()->getCompositionParameterModel(id) : nullptr;
    }
    return std::dynamic_pointer_cast<EffectItemModel>(stackItem(effectStack(args), args));
}

QJsonObject McpTools::assetInfo(const std::shared_ptr<AssetParameterModel> &model)
{
    QJsonArray params;
    const auto animated = model->getKeyframableParameters();
    for (int row = 0; row < model->rowCount(); ++row) {
        const auto index = model->index(row, 0);
        const QString name = model->data(index, AssetParameterModel::NameRole).toString();
        params.append(QJsonObject{{"name", name},
                                  {"value", model->getParamFromName(name).toString()},
                                  {"type", int(model->data(index, AssetParameterModel::TypeRole).value<ParamType>())},
                                  {"display_name", model->data(index, Qt::DisplayRole).toString()},
                                  {"minimum", QJsonValue::fromVariant(model->data(index, AssetParameterModel::MinRole))},
                                  {"maximum", QJsonValue::fromVariant(model->data(index, AssetParameterModel::MaxRole))},
                                  {"choices", QJsonValue::fromVariant(model->data(index, AssetParameterModel::ListValuesRole))},
                                  {"keyframable", animated.contains(name)}});
    }
    const auto index = model->index(0, 0);
    return {{"asset_id", model->getAssetId()},
            {"parameters", params},
            {"source_in", model->data(index, AssetParameterModel::ParentInRole).toInt()},
            {"duration", model->data(index, AssetParameterModel::ParentDurationRole).toInt()}};
}

void McpTools::registerAssetTools()
{
    const QJsonObject target{{"target", targetSchema()}};
    const auto selected = with(target, {{"effect_row", integer()}, {"effect_path", array(integer(), 1, 16)}});
    const auto frame = integer(0, 1000000000);

    add("assets_list", "Search all installed effects or transitions, including audio, color, transform, masks and custom assets. No fixed essential subset.",
        {{"kind", enumeration({"effect", "transition"})}, {"query", string()}, {"offset", integer()}, {"limit", integer(1, 1000)}}, {"kind"}, true,
        [](const QJsonObject &a) {
            const bool effects = a["kind"] == QLatin1String("effect");
            auto names = effects ? EffectsRepository::get()->getNames() : TransitionsRepository::get()->getNames();
            std::sort(names.begin(), names.end());
            QJsonArray result;
            const QString query = a["query"].toString();
            int count = 0;
            const int offset = a["offset"].toInt(), limit = a["limit"].toInt(100);
            for (const auto &entry : names) {
                if (!entry.first.contains(query, Qt::CaseInsensitive) && !entry.second.contains(query, Qt::CaseInsensitive)) {
                    continue;
                }
                if (count >= offset && result.size() < limit) {
                    result.append(QJsonObject{{"asset_id", entry.first}, {"name", entry.second}});
                }
                ++count;
            }
            return success({{"assets", result}, {"total", count}});
        });

    add("assets_describe", "Read the complete installed asset XML with parameters, ranges, defaults and description before applying an effect/transition.",
        {{"kind", enumeration({"effect", "transition"})}, {"asset_id", string()}}, {"kind", "asset_id"}, true, [](const QJsonObject &a) {
            const QString id = a["asset_id"].toString();
            const bool effect = a["kind"] == QLatin1String("effect");
            if (!(effect ? EffectsRepository::get()->exists(id) : TransitionsRepository::get()->exists(id))) {
                return failure("Unknown asset.");
            }
            QDomDocument doc;
            doc.appendChild(doc.importNode(effect ? EffectsRepository::get()->getXml(id) : TransitionsRepository::get()->getXml(id), true));
            return success({{"asset_id", id},
                            {"xml", doc.toString()},
                            {"description", effect ? EffectsRepository::get()->getDescription(id) : TransitionsRepository::get()->getDescription(id)}});
        });

    add("effects_list",
        "Read the entire effect stack of a clip, track, master or bin clip. Nested effects return effect_path for addressing. Mixes use asset_parameters_get "
        "directly.",
        target, {"target"}, true, [](const QJsonObject &a) {
            const auto stack = effectStack(a);
            if (!stack) {
                return failure("No effect stack for this target.");
            }
            QJsonArray result;
            std::function<void(std::shared_ptr<TreeItem>, QJsonArray)> visit = [&](std::shared_ptr<TreeItem> parent, QJsonArray path) {
                const int count = parent ? parent->childCount() : stack->rowCount();
                for (int row = 0; row < count; ++row) {
                    const auto item = stack->getEffectStackRow(row, parent);
                    auto itemPath = path;
                    itemPath.append(row);
                    auto effect = std::dynamic_pointer_cast<EffectItemModel>(item);
                    QJsonObject info = effect ? assetInfo(effect) : QJsonObject{{"group", true}};
                    info.insert("effect_path", itemPath);
                    info.insert("enabled", item->isAssetEnabled());
                    if (path.isEmpty()) {
                        info.insert("effect_row", row);
                    }
                    result.append(info);
                    if (!effect) {
                        visit(item, itemPath);
                    }
                }
            };
            visit(nullptr, {});
            return success({{"enabled", stack->isStackEnabled()}, {"effects", result}});
        });

    add("effect_add", "Append any installed effect to a clip, track, master or bin stack. Use asset_parameters_get/set afterwards. Undoable.",
        with(target, {{"asset_id", string()}}), {"target", "asset_id"}, false, [](const QJsonObject &a) {
            const auto stack = effectStack(a);
            const QString id = a["asset_id"].toString();
            if (!stack || !EffectsRepository::get()->exists(id)) {
                return failure("Unknown target or effect.");
            }
            if (!stack->appendEffect(id, true)) {
                return failure("Effect insertion rejected (incompatible or duplicate unique effect).");
            }
            return success({{"effect_row", stack->rowCount() - 1}});
        });

    add("effect_remove", "Remove an addressed effect, including an effect inside a group. Undoable.", selected, {"target"}, false, [](const QJsonObject &a) {
        const auto stack = effectStack(a);
        const auto effect = std::dynamic_pointer_cast<EffectItemModel>(stackItem(stack, a));
        if (!effect) {
            return failure("Unknown effect. Supply effect_row or effect_path from effects_list.");
        }
        if (effect->isBuiltIn()) {
            return failure("Built-in effects must be disabled instead of removed.");
        }
        stack->removeEffect(effect);
        return success();
    });

    add("effect_move", "Reorder a top-level effect in the stack to a zero-based destination row. Undoable.",
        with(target, {{"effect_row", integer()}, {"destination_row", integer()}}), {"target", "effect_row", "destination_row"}, false,
        [](const QJsonObject &a) {
            const auto stack = effectStack(a);
            const auto item = std::dynamic_pointer_cast<EffectItemModel>(stackItem(stack, a));
            const int row = a["destination_row"].toInt();
            if (!item || row >= stack->rowCount()) {
                return failure("Invalid effect or destination row.");
            }
            if (item->isBuiltIn()) {
                return failure("Built-in effects cannot be reordered.");
            }
            if (row == item->row()) {
                return success();
            }
            if (row < item->row()) {
                const auto destination = std::dynamic_pointer_cast<EffectItemModel>(stack->getEffectStackRow(row));
                if (!destination || destination->isBuiltIn()) {
                    return failure("Cannot move before this effect group or built-in effect.");
                }
            }
            // The model takes a drop boundary; the API takes the final row.
            stack->moveEffect(row > item->row() ? row + 1 : row, item);
            return booleanResult(item->row() == row, "The effect could not be moved.");
        });

    add("effect_enable", "Enable/disable an addressed effect or effect group. Undoable.", with(selected, {{"enabled", boolean()}}), {"target", "enabled"},
        false, [](const QJsonObject &a) {
            const auto item = stackItem(effectStack(a), a);
            if (!item) {
                return failure("Unknown effect.");
            }
            Fun undo = []() { return true; }, redo = undo;
            item->markEnabled(a["enabled"].toBool(), undo, redo);
            pCore->pushUndo(undo, redo, QStringLiteral("Set effect enabled"));
            return success();
        });

    add("effects_enable", "Enable/disable the entire target effect stack. Undoable.", with(target, {{"enabled", boolean()}}), {"target", "enabled"}, false,
        [](const QJsonObject &a) {
            const auto stack = effectStack(a);
            if (!stack) {
                return failure("Unknown effect stack.");
            }
            const bool old = stack->isStackEnabled(), enabled = a["enabled"].toBool();
            Fun undo = [stack, old]() {
                stack->setEffectStackEnabled(old);
                return true;
            };
            Fun redo = [stack, enabled]() {
                stack->setEffectStackEnabled(enabled);
                return true;
            };
            redo();
            pCore->pushUndo(undo, redo, QStringLiteral("Set effect stack enabled"));
            return success();
        });

    add("effect_set_zone", "Restrict an effect to inclusive source-frame in/out bounds, or disable its zone. Undoable.",
        with(selected, {{"in", frame}, {"out", frame}, {"enabled", boolean()}}), {"target", "in", "out", "enabled"}, false, [](const QJsonObject &a) {
            const auto effect = std::dynamic_pointer_cast<EffectItemModel>(stackItem(effectStack(a), a));
            if (!effect || a["in"].toInt() > a["out"].toInt()) {
                return failure("Unknown effect or invalid range.");
            }
            effect->setInOut(effect->getAssetId(), {a["in"].toInt(), a["out"].toInt()}, a["enabled"].toBool(), true);
            return success();
        });

    add("effects_copy", "Copy all effects from source target to destination target, preserving parameters and animation. Undoable.",
        {{"source", targetSchema()}, {"target", targetSchema()}}, {"source", "target"}, false, [](const QJsonObject &a) {
            const auto source = effectStack({{"target", a["source"]}}), destination = effectStack(a);
            if (!source || !destination || source == destination) {
                return failure("Expected two distinct effect stacks.");
            }
            PlaylistState::ClipState state = PlaylistState::Disabled;
            const auto target = a["target"].toObject();
            if (target["kind"] == QLatin1String("clip")) {
                state = timeline()->getClipState(target["item_id"].toInt()).first;
            }
            return booleanResult(destination->importEffects(source, state));
        });

    add("asset_parameters_get",
        "Inspect all live parameters on an effect or composition. For effects supply effect_row or effect_path; compositions need only target.", selected,
        {"target"}, true, [](const QJsonObject &a) {
            const auto model = asset(a);
            return model ? success(assetInfo(model)) : failure("Unknown asset target/effect.");
        });

    add("asset_parameters_set",
        "Set multiple named effect/composition parameters as one undo command. Values use the asset's native units/MLT strings; inspect assets_describe first.",
        with(selected, {{"parameters", array(object({{"name", string()}, {"value", string()}}, {"name", "value"}), 1, 256)}}), {"target", "parameters"}, false,
        [](const QJsonObject &a) {
            const auto model = asset(a);
            if (!model) {
                return failure("Unknown asset target/effect.");
            }
            QList<QModelIndex> indexes;
            QStringList values;
            for (const auto &entry : a["parameters"].toArray()) {
                const auto param = entry.toObject();
                const auto index = model->getParamIndexFromName(param["name"].toString());
                if (!index.isValid() || indexes.contains(index) ||
                    model->data(index, AssetParameterModel::TypeRole).value<ParamType>() == ParamType::Readonly) {
                    return failure("Unknown, duplicate or read-only parameter: " + param["name"].toString());
                }
                indexes.append(index);
                values.append(param["value"].toString());
            }
            pCore->pushUndo(new AssetMultiCommand(model, indexes, values));
            return success(assetInfo(model));
        });

    add("keyframe_types", "List every interpolation mode supported by this Kdenlive build.", {}, {}, true, [](const QJsonObject &) {
        const auto enumeration = QMetaEnum::fromType<KeyframeType::KeyframeEnum>();
        QJsonArray types;
        for (int i = 0; i < enumeration.keyCount(); ++i) {
            types.append(QString::fromLatin1(enumeration.key(i)));
        }
        return success({{"types", types}});
    });

    add("keyframes_get", "Inspect all keyframes and their native parameter values. Keyframe times are source frames (source_in + clip-relative frame).",
        selected, {"target"}, true, [](const QJsonObject &a) {
            const auto model = asset(a);
            if (!model) {
                return failure("Unknown asset.");
            }
            model->prepareKeyframes();
            const auto keys = model->getKeyframeModel();
            if (!keys) {
                return failure("Asset has no animated parameters.");
            }
            QJsonArray frames;
            for (int i = 0; i < keys->count(); ++i) {
                const auto pos = keys->getPosAtIndex(i);
                QJsonObject values;
                for (const auto &index : keys->getIndexes()) {
                    values.insert(model->data(index, AssetParameterModel::NameRole).toString(),
                                  QJsonValue::fromVariant(keys->getInterpolatedValue(pos, index)));
                }
                frames.append(QJsonObject{{"frame", pos.frames(pCore->getCurrentFps())}, {"type", int(keys->keyframeType(pos))}, {"values", values}});
            }
            return success({{"keyframes", frames}, {"asset", assetInfo(model)}});
        });

    add("keyframe_add", "Add a keyframe across all animated parameters using interpolated values. Existing frames update interpolation. Undoable.",
        with(selected, {{"frame", frame}, {"type", string("Name returned by keyframe_types; default Linear.")}}), {"target", "frame"}, false,
        [](const QJsonObject &a) {
            const auto model = asset(a);
            if (!model) {
                return failure("Unknown asset.");
            }
            const auto error = keyframeRangeError(model, a["frame"].toInt());
            if (!error.isEmpty()) {
                return failure(error);
            }
            const auto enumeration = QMetaEnum::fromType<KeyframeType::KeyframeEnum>();
            bool ok = false;
            const int type = enumeration.keyToValue(a["type"].toString(QStringLiteral("Linear")).toLatin1().constData(), &ok);
            if (!ok) {
                return failure("Unknown keyframe interpolation type.");
            }
            model->prepareKeyframes();
            const auto keys = model->getKeyframeModel();
            return keys ? booleanResult(keys->addKeyframe(GenTime(a["frame"].toInt(), pCore->getCurrentFps()), static_cast<KeyframeType::KeyframeEnum>(type)))
                        : failure("Asset has no animated parameters.");
        });

    add("keyframe_update", "Update one parameter at an existing keyframe. Value is a native parameter string (number, geometry, color, etc.). Undoable.",
        with(selected, {{"frame", frame}, {"parameter", string()}, {"value", string()}}), {"target", "frame", "parameter", "value"}, false,
        [](const QJsonObject &a) {
            const auto model = asset(a);
            if (!model || !model->getKeyframableParameters().contains(a["parameter"].toString())) {
                return failure("Unknown animated parameter.");
            }
            model->prepareKeyframes();
            const auto keys = model->getKeyframeModel();
            if (!keys || !keys->hasKeyframe(a["frame"].toInt())) {
                return failure("Add the keyframe before updating its value.");
            }
            const auto index = model->getParamIndexFromName(a["parameter"].toString());
            return booleanResult(keys->updateKeyframe(GenTime(a["frame"].toInt(), pCore->getCurrentFps()), a["value"].toString(), -1, index));
        });

    add("keyframe_remove", "Remove a keyframe across all animated parameters. The initial keyframe is protected. Undoable.", with(selected, {{"frame", frame}}),
        {"target", "frame"}, false, [](const QJsonObject &a) {
            const auto model = asset(a);
            if (!model) {
                return failure("Unknown asset.");
            }
            model->prepareKeyframes();
            const auto keys = model->getKeyframeModel();
            if (keys && keys->count() > 0 && keys->getPosAtIndex(0).frames(pCore->getCurrentFps()) == a["frame"].toInt()) {
                return failure("The initial keyframe cannot be removed.");
            }
            return keys && keys->hasKeyframe(a["frame"].toInt()) ? booleanResult(keys->removeKeyframe(GenTime(a["frame"].toInt(), pCore->getCurrentFps())))
                                                                 : failure("Unknown keyframe.");
        });

    add("keyframe_move", "Move a keyframe across all animated parameters to a new source frame. Undoable.",
        with(selected, {{"frame", frame}, {"new_frame", frame}}), {"target", "frame", "new_frame"}, false, [](const QJsonObject &a) {
            const auto model = asset(a);
            if (!model) {
                return failure("Unknown asset.");
            }
            const auto error = keyframeRangeError(model, a["new_frame"].toInt());
            if (!error.isEmpty()) {
                return failure(error);
            }
            model->prepareKeyframes();
            const auto keys = model->getKeyframeModel();
            if (a["frame"].toInt() == a["new_frame"].toInt()) {
                return keys && keys->hasKeyframe(a["frame"].toInt()) ? success() : failure("Unknown keyframe.");
            }
            if (keys && ((keys->count() > 0 && keys->getPosAtIndex(0).frames(pCore->getCurrentFps()) == a["frame"].toInt()) ||
                         keys->hasKeyframe(a["new_frame"].toInt()))) {
                return failure("Cannot move the initial keyframe or overwrite another keyframe.");
            }
            return keys && keys->hasKeyframe(a["frame"].toInt())
                       ? booleanResult(keys->moveKeyframe(GenTime(a["frame"].toInt(), pCore->getCurrentFps()),
                                                          GenTime(a["new_frame"].toInt(), pCore->getCurrentFps()), true))
                       : failure("Unknown keyframe.");
        });

    add("asset_sample", "Evaluate interpolated animated asset parameters at a source frame.", with(selected, {{"frame", frame}}), {"target", "frame"}, true,
        [](const QJsonObject &a) {
            const auto model = asset(a);
            if (!model) {
                return failure("Unknown asset.");
            }
            const auto json = model->valueAsJson(a["frame"].toInt());
            return success({{"values", json.isArray() ? QJsonValue(json.array()) : QJsonValue(json.object())}});
        });

    add("clip_fade", "Set a clip's audio/video fade-in or fade-out duration. Audio gain and all other processing are available through effects. Undoable.",
        {{"item_id", integer()}, {"duration", integer(1, 100000000)}, {"from_start", boolean()}, {"audio", boolean()}, {"video", boolean()}},
        {"item_id", "duration", "from_start", "audio", "video"}, false, [](const QJsonObject &a) {
            const int id = a["item_id"].toInt();
            if (!timeline()->isClip(id) || a["duration"].toInt() > timeline()->getClipPlaytime(id) || (!a["audio"].toBool() && !a["video"].toBool())) {
                return failure("Invalid fade or clip.");
            }
            return booleanResult(timeline()->getClipEffectStackModel(id)->adjustFadeLength(a["duration"].toInt(), a["from_start"].toBool(), a["audio"].toBool(),
                                                                                           a["video"].toBool(), true));
        });
}
