// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#include "util/json.h"

#include <string>
#include <vector>

#include "column/column.h"
#include "common/status.h"
#include "common/statusor.h"
#include "gutil/strings/substitute.h"
#include "simdjson.h"
#include "velocypack/ValueType.h"
#include "velocypack/vpack.h"

namespace starrocks {

Status JsonValue::parse(const Slice& src, JsonValue* out) {
    try {
        if (src.empty()) {
            *out = JsonValue(noneJsonSlice());
            return Status::OK();
        }
        auto b = vpack::Parser::fromJson(src.get_data(), src.get_size());
        out->assign(*b);
    } catch (const vpack::Exception& e) {
        return fromVPackException(e);
    }
    return Status::OK();
}

JsonValue JsonValue::from_null() {
    return JsonValue(nullJsonSlice());
}

JsonValue JsonValue::from_int(int64_t value) {
    vpack::Builder builder;
    builder.add(vpack::Value(value));
    return JsonValue(builder.slice());
}

JsonValue JsonValue::from_uint(uint64_t value) {
    vpack::Builder builder;
    builder.add(vpack::Value(value));
    return JsonValue(builder.slice());
}

JsonValue JsonValue::from_bool(bool value) {
    vpack::Builder builder;
    builder.add(vpack::Value(value));
    return JsonValue(builder.slice());
}

JsonValue JsonValue::from_double(double value) {
    vpack::Builder builder;
    builder.add(vpack::Value(value));
    return JsonValue(builder.slice());
}

JsonValue JsonValue::from_string(const Slice& value) {
    vpack::Builder builder;
    builder.add(vpack::Value(value.to_string()));
    return JsonValue(builder.slice());
}

StatusOr<JsonValue> JsonValue::from_simdjson(simdjson::ondemand::value* value) {
    namespace sj = simdjson::ondemand;

    try {
        sj::json_type tp = value->type();
        switch (tp) {
        case sj::json_type::null: {
            return from_null();
        }
        case sj::json_type::number: {
            switch (value->get_number_type()) {
            case sj::number_type::signed_integer:
                return from_int(value->get_int64());
            case sj::number_type::unsigned_integer:
                return from_uint(value->get_uint64());
            case sj::number_type::floating_point_number:
                return from_double(value->get_double());
            }
        }

        case sj::json_type::string: {
            std::string_view view = value->get_string();
            return from_string(Slice(view.data(), view.size()));
        }

        case sj::json_type::boolean: {
            return from_bool(value->get_bool());
        }

        case sj::json_type::array:
        case sj::json_type::object: {
            // TODO(mofei) optimize this, avoid convert to string then parse it
            std::string_view view = simdjson::to_json_string(*value);
            return parse(Slice(view.data(), view.size()));
        }

        default: {
            auto err_msg = strings::Substitute("Unsupported json type: $0", int(tp));
            return Status::DataQualityError(err_msg);
        }
        }
    } catch (simdjson::simdjson_error& e) {
        std::string_view view = simdjson::to_json_string(*value);
        auto err_msg = strings::Substitute("Failed to parse value, json=$0, error=$1", view.data(),
                                           simdjson::error_message(e.error()));
        return Status::DataQualityError(err_msg);
    }

    return Status::OK();
}

StatusOr<JsonValue> JsonValue::from_simdjson(simdjson::ondemand::object* obj) {
    // TODO(mofei) optimize this, avoid convert to string then parse it
    std::string_view view = obj->raw_json();
    try {
        return parse(Slice(view.data(), view.size()));
    } catch (simdjson::simdjson_error& e) {
        auto err_msg = strings::Substitute("Failed to parse value, json=$0, error=$1", view.data(),
                                           simdjson::error_message(e.error()));
        return Status::DataQualityError(err_msg);
    }
}

StatusOr<JsonValue> JsonValue::parse(const Slice& src) {
    JsonValue json;
    RETURN_IF_ERROR(parse(src, &json));
    return json;
}

size_t JsonValue::serialize(uint8_t* dst) const {
    memcpy(dst, binary_.data(), binary_.size());
    return serialize_size();
}

uint64_t JsonValue::serialize_size() const {
    return binary_.size();
}

// NOTE: JsonValue must be a valid JSON, which means to_string should not fail
StatusOr<std::string> JsonValue::to_string() const {
    if (binary_.empty()) {
        return "";
    }
    return callVPack<std::string>([this]() {
        VSlice slice = to_vslice();
        vpack::Options options = vpack::Options::Defaults;
        options.singleLinePrettyPrint = true;

        std::string result;
        return slice.toJson(result, &options);
    });
}

std::string JsonValue::to_string_uncheck() const {
    auto res = to_string();
    if (res.ok()) {
        return res.value();
    } else {
        return "";
    }
}

vpack::Slice JsonValue::to_vslice() const {
    return vpack::Slice((const uint8_t*)binary_.data());
}

static inline int cmpDouble(double left, double right) {
    if (std::isless(left, right)) {
        return -1;
    } else if (std::isgreater(left, right)) {
        return 1;
    }
    return 0;
}

static int sliceCompare(const vpack::Slice& left, const vpack::Slice& right) {
    if (left.isObject() && right.isObject()) {
        for (auto it : vpack::ObjectIterator(left)) {
            auto sub = right.get(it.key.stringRef());
            if (!sub.isNone()) {
                int x = sliceCompare(it.value, sub);
                if (x != 0) {
                    return x;
                }
            } else {
                return 1;
            }
        }
        return 0;
    } else if (left.isArray() && right.isArray()) {
        int idx = 0;
        for (auto it : vpack::ArrayIterator(left)) {
            auto sub = right.at(idx);
            if (!sub.isNone()) {
                int x = sliceCompare(it, sub);
                if (x != 0) {
                    return x;
                }
            }
            idx++;
        }
        return 0;
    } else if (vpack::valueTypeGroup(left.type()) == vpack::valueTypeGroup(right.type())) {
        // 1. type are exactly same
        // 2. type are both number, but could smallInt/Int/Double
        if (left.type() == right.type()) {
            switch (left.type()) {
            case vpack::ValueType::Bool:
                return left.getBool() - right.getBool();
            case vpack::ValueType::SmallInt:
            case vpack::ValueType::Int:
            case vpack::ValueType::UInt:
                return left.getInt() - right.getInt();
            case vpack::ValueType::Double: {
                return cmpDouble(left.getDouble(), right.getDouble());
            }
            case vpack::ValueType::String:
                return left.stringRef().compare(right.stringRef());
            default:
                // other types like illegal, none, min, max are considered equal
                return 0;
            }
        } else if (left.isInteger() && right.isInteger()) {
            return left.getInt() - right.getInt();
        } else {
            return cmpDouble(left.getNumber<double>(), right.getNumber<double>());
        }
    } else {
        if (left.type() == vpack::ValueType::MinKey) {
            return -1;
        }
        if (right.type() == vpack::ValueType::MinKey) {
            return 1;
        }
        if (left.type() == vpack::ValueType::MaxKey) {
            return 1;
        }
        if (right.type() == vpack::ValueType::MaxKey) {
            return -1;
        }
        return (int)left.type() - (int)right.type();
    }
    return 0;
}

int JsonValue::compare(const JsonValue& rhs) const {
    auto left = to_vslice();
    auto right = rhs.to_vslice();
    return sliceCompare(left, right);
}

int JsonValue::compare(const Slice& lhs, const Slice& rhs) {
    vpack::Slice ls;
    if (lhs.size > 0) {
        ls = vpack::Slice((const uint8_t*)lhs.data);
    } else {
        ls = vpack::Slice::noneSlice();
    }
    vpack::Slice rs;
    if (rhs.size > 0) {
        rs = vpack::Slice((const uint8_t*)rhs.data);
    } else {
        rs = vpack::Slice::noneSlice();
    }

    return sliceCompare(ls, rs);
}

int64_t JsonValue::hash() const {
    return to_vslice().normalizedHash();
}

Slice JsonValue::get_slice() const {
    return Slice(binary_);
}

JsonType JsonValue::get_type() const {
    return fromVPackType(to_vslice().type());
}

StatusOr<bool> JsonValue::get_bool() const {
    return callVPack<bool>([this]() { return to_vslice().getBool(); });
}

StatusOr<int64_t> JsonValue::get_int() const {
    return callVPack<int64_t>([this]() { return to_vslice().getNumber<int64_t>(); });
}

StatusOr<uint64_t> JsonValue::get_uint() const {
    return callVPack<uint64_t>([this]() { return to_vslice().getNumber<uint64_t>(); });
}

StatusOr<double> JsonValue::get_double() const {
    return callVPack<double>([this]() { return to_vslice().getNumber<double>(); });
}

StatusOr<Slice> JsonValue::get_string() const {
    return callVPack<Slice>([this]() {
        vpack::ValueLength len;
        const char* str = to_vslice().getString(len);
        return Slice(str, len);
    });
}

bool JsonValue::is_null() const {
    return to_vslice().isNull();
}

std::ostream& operator<<(std::ostream& os, const JsonValue& json) {
    return os << json.to_string_uncheck();
}

} //namespace starrocks