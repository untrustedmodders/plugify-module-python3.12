#include "module.hpp"
#include <array>
#include <climits>
#include <cuchar>
#include <module_export.h>
#include <plugify/compat_format.hpp>
#include <plugify/log.hpp>
#include <plugify/math.hpp>
#include <plugify/module.hpp>
#include <plugify/plugify_provider.hpp>
#include <plugify/plugin.hpp>
#include <plugify/plugin_descriptor.hpp>
#include <plugify/string.hpp>

using namespace plugify;
namespace fs = std::filesystem;

namespace py3lm {
	extern Python3LanguageModule g_py3lm;

	namespace {
		void ReplaceAll(std::string& str, const std::string& from, const std::string& to) {
			size_t start_pos{};
			while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
				str.replace(start_pos, from.length(), to);
				start_pos += to.length();
			}
		}

		bool IsStaticMethod(PyObject* obj) {
			if (PyCFunction_Check(obj)) {
				PyCFunctionObject* cfunc = reinterpret_cast<PyCFunctionObject*>(obj);
				if (cfunc->m_ml && (cfunc->m_ml->ml_flags & METH_STATIC)) {
					return true;
				}
			}
			return false;
		}

		// Return codes:
		// [1, 3]	Number bytes used
		// 0		Sequence starts with \0
		// -1		Encoding error
		// -2		Invalid multibyte sequence
		// -3		Surrogate pair
		std::pair<short, char16_t> ConvertUtf8ToUtf16(std::span<const char> sequence) {
			const auto c8toc16 = [](char ch) -> char16_t { return static_cast<char16_t>(static_cast<uint8_t>(ch)); };

			if (sequence.empty()) {
				return { -2, {} };
			}
			const char seqCh0 = sequence[0];
			if (seqCh0 == '\0') {
				return { 0, 0 };
			}
			if ((seqCh0 & 0b11111000) == 0b11110000) {
				return { -3, {} };
			}
			if ((seqCh0 & 0b11110000) == 0b11100000) {
				if (sequence.size() < 3) {
					return { -2, {} };
				}
				const char seqCh1 = sequence[1];
				const char seqCh2 = sequence[2];
				if ((seqCh1 & 0b11000000) != 0b10000000 || (seqCh2 & 0b11000000) != 0b10000000) {
					return { -2, {} };
				}
				const char16_t ch = (c8toc16(seqCh0 & 0b00001111) << 12) | (c8toc16(seqCh1 & 0b00111111) << 6) | c8toc16(seqCh2 & 0b00111111);
				if (0xD800 <= static_cast<uint16_t>(ch) && static_cast<uint16_t>(ch) < 0xE000) {
					return { -1, {} };
				}
				return { 3, ch };
			}
			if ((seqCh0 & 0b11100000) == 0b11000000) {
				if (sequence.size() < 2) {
					return { -2, {} };
				}
				const char seqCh1 = sequence[1];
				if ((seqCh1 & 0b11000000) != 0b10000000) {
					return { -2, {} };
				}
				return { 2, (c8toc16(seqCh0 & 0b00011111) << 6) | c8toc16(seqCh1 & 0b00111111) };
			}
			if ((seqCh0 & 0b10000000) == 0b00000000) {
				return { 1, c8toc16(seqCh0) };
			}
			return { -1, {} };
		}

		// Return codes:
		// [1, 3]	Number bytes returned
		// 0		For 0x0000 symbol
		// -1		Surrogate pair
		std::pair<short, std::array<char, 4>> ConvertUtf16ToUtf8(char16_t ch16) {
			const auto c16toc8 = [](char16_t ch) -> char { return static_cast<char>(static_cast<uint8_t>(ch)); };

			if (ch16 == 0) {
				return { 0, { } };
			}
			if (static_cast<uint16_t>(ch16) < 0x80) {
				return { 1, { c16toc8(ch16), '\0' } };
			}
			if (static_cast<uint16_t>(ch16) < 0x800) {
				return { 2, { c16toc8(((ch16 & 0b11111000000) >> 6) | 0b11000000), c16toc8((ch16 & 0b111111) | 0b10000000), '\0' } };
			}
			if (0xD800 <= static_cast<uint16_t>(ch16) && static_cast<uint16_t>(ch16) < 0xE000) {
				return { -1, {} };
			}
			return { 3, { c16toc8(((ch16 & 0b1111000000000000) >> 12) | 0b11100000), c16toc8(((ch16 & 0b111111000000) >> 6) | 0b10000000), c16toc8((ch16 & 0b111111) | 0b10000000), '\0' } };
		}

		using MethodExportError = std::string;
		using MethodExportData = PythonMethodData;
		using MethodExportResult = std::variant<MethodExportError, MethodExportData>;

		template<class T>
		inline constexpr bool always_false_v = std::is_same_v<std::decay_t<T>, std::add_cv_t<std::decay_t<T>>>;

		template<typename T>
		std::optional<T> ValueFromObject(PyObject* /*object*/) {
			static_assert(always_false_v<T>, "ValueFromObject specialization required");
		}

		template<>
		std::optional<bool> ValueFromObject(PyObject* object) {
			if (PyBool_Check(object)) {
				return { object == Py_True };
			}
			PyErr_SetString(PyExc_TypeError, "Not boolean");
			return std::nullopt;
		}

		template<>
		std::optional<char> ValueFromObject(PyObject* object) {
			if (PyUnicode_Check(object)) {
				const Py_ssize_t length = PyUnicode_GetLength(object);
				if (length == 0) {
					return { 0 };
				}
				if (length == 1) {
					char ch = PyUnicode_AsUTF8(object)[0];
					if ((ch & 0x80) == 0) {
						return { ch };
					}
					// Can't pass multibyte character
					PyErr_SetString(PyExc_ValueError, "Multibyte character");
				}
				else {
					PyErr_SetString(PyExc_ValueError, "Length bigger than 1");
				}
				return std::nullopt;
			}
			PyErr_SetString(PyExc_TypeError, "Not string");
			return std::nullopt;
		}

		template<>
		std::optional<char16_t> ValueFromObject(PyObject* object) {
			if (PyUnicode_Check(object)) {
				const Py_ssize_t length = PyUnicode_GetLength(object);
				if (length == 0) {
					return { 0 };
				}
				if (length == 1) {
					Py_ssize_t size{};
					const char* const buffer = PyUnicode_AsUTF8AndSize(object, &size);
					auto [rc, ch] = ConvertUtf8ToUtf16({ buffer, static_cast<std::size_t>(size) });
					switch (rc) {
					case 0:
					case 1:
					case 2:
					case 3:
						return { ch };
					case -3:
						PyErr_SetString(PyExc_ValueError, "Surrogate pair");
						break;
					case -2:
						PyErr_SetString(PyExc_ValueError, "Invalid multibyte character");
						break;
					case -1:
						PyErr_SetString(PyExc_RuntimeError, "Encoding error");
						break;
					}
				}
				else {
					PyErr_SetString(PyExc_ValueError, "Length bigger than 1");
				}
				return std::nullopt;
			}
			PyErr_SetString(PyExc_TypeError, "Not string");
			return std::nullopt;
		}

		template<class ValueType, class CType, CType (*ConvertFunc)(PyObject*)>
		std::optional<ValueType> ValueFromNumberObject(PyObject* object) {
			if (PyLong_Check(object)) {
				const CType castResult = ConvertFunc(object);
				if (!PyErr_Occurred()) {
					if (castResult <= static_cast<CType>(std::numeric_limits<ValueType>::max())
						&& castResult >= static_cast<CType>(std::numeric_limits<ValueType>::min())
					) {
						return { static_cast<ValueType>(castResult) };
					}
					PyErr_SetNone(PyExc_OverflowError);
				}
				return std::nullopt;
			}
			PyErr_SetString(PyExc_TypeError, "Not integer");
			return std::nullopt;
		}

		template<class ValueType, auto ConvertFunc>
		auto ValueFromNumberObject(PyObject* object) {
			return ValueFromNumberObject<ValueType, std::invoke_result_t<decltype(ConvertFunc), PyObject*>, ConvertFunc>(object);
		}

		template<>
		std::optional<int8_t> ValueFromObject(PyObject* object) {
			return ValueFromNumberObject<int8_t, PyLong_AsLong>(object);
		}

		template<>
		std::optional<int16_t> ValueFromObject(PyObject* object) {
			return ValueFromNumberObject<int16_t, PyLong_AsLong>(object);
		}

		template<>
		std::optional<int32_t> ValueFromObject(PyObject* object) {
			return ValueFromNumberObject<int32_t, PyLong_AsLong>(object);
		}

		template<>
		std::optional<int64_t> ValueFromObject(PyObject* object) {
			return ValueFromNumberObject<int64_t, PyLong_AsLongLong>(object);
		}

		template<>
		std::optional<uint8_t> ValueFromObject(PyObject* object) {
			return ValueFromNumberObject<uint8_t, PyLong_AsUnsignedLong>(object);
		}

		template<>
		std::optional<uint16_t> ValueFromObject(PyObject* object) {
			return ValueFromNumberObject<uint16_t, PyLong_AsUnsignedLong>(object);
		}

		template<>
		std::optional<uint32_t> ValueFromObject(PyObject* object) {
			return ValueFromNumberObject<uint32_t, PyLong_AsUnsignedLong>(object);
		}

		template<>
		std::optional<uint64_t> ValueFromObject(PyObject* object) {
			return ValueFromNumberObject<uint64_t, PyLong_AsUnsignedLongLong>(object);
		}

		template<>
		std::optional<void*> ValueFromObject(PyObject* object) {
			if (PyLong_Check(object)) {
				const auto result = PyLong_AsVoidPtr(object);
				if (!PyErr_Occurred()) {
					return result;
				}
			}
			PyErr_SetString(PyExc_TypeError, "Not integer");
			return std::nullopt;
		}

		template<>
		std::optional<float> ValueFromObject(PyObject* object) {
			if (PyFloat_Check(object)) {
				return static_cast<float>(PyFloat_AS_DOUBLE(object));
			}
			PyErr_SetString(PyExc_TypeError, "Not float");
			return std::nullopt;
		}

		template<>
		std::optional<double> ValueFromObject(PyObject* object) {
			if (PyFloat_Check(object)) {
				return PyFloat_AS_DOUBLE(object);
			}
			PyErr_SetString(PyExc_TypeError, "Not float");
			return std::nullopt;
		}

		template<>
		std::optional<plg::string> ValueFromObject(PyObject* object) {
			if (PyUnicode_Check(object)) {
				Py_ssize_t size{};
				const char* const buffer = PyUnicode_AsUTF8AndSize(object, &size);
				return std::make_optional<plg::string>(buffer, buffer + size);
			}
			PyErr_SetString(PyExc_TypeError, "Not string");
			return std::nullopt;
		}

		template<>
		std::optional<Vector2> ValueFromObject(PyObject* object) {
			return g_py3lm.Vector2ValueFromObject(object);
		}

		template<>
		std::optional<Vector3> ValueFromObject(PyObject* object) {
			return g_py3lm.Vector3ValueFromObject(object);
		}

		template<>
		std::optional<Vector4> ValueFromObject(PyObject* object) {
			return g_py3lm.Vector4ValueFromObject(object);
		}

		template<>
		std::optional<Matrix4x4> ValueFromObject(PyObject* object) {
			return g_py3lm.Matrix4x4ValueFromObject(object);
		}

		template<typename T>
		std::optional<plg::vector<T>> ArrayFromObject(PyObject* arrayObject) {
			if (!PyList_Check(arrayObject)) {
				PyErr_SetString(PyExc_TypeError, "Not list");
				return std::nullopt;
			}
			const Py_ssize_t size = PyList_Size(arrayObject);
			plg::vector<T> array(static_cast<size_t>(size));
			for (Py_ssize_t i = 0; i < size; ++i) {
				if (PyObject* const valueObject = PyList_GetItem(arrayObject, i)) {
					if (auto value = ValueFromObject<T>(valueObject)) {
						array[static_cast<size_t>(i)] = std::move(*value);
						continue;
					}
				}
				return std::nullopt;
			}
			return array;
		}

		std::optional<void*> GetOrCreateFunctionValue(MethodRef method, PyObject* object) {
			return g_py3lm.GetOrCreateFunctionValue(method, object);
		}

		template<typename T>
		void* CreateValue(PyObject* pItem) {
			auto value = ValueFromObject<T>(pItem);
			if (value) {
				return new T(std::move(*value));
			}
			return nullptr;
		}

		template<typename T>
		void* CreateArray(PyObject* pItem) {
			auto array = ArrayFromObject<T>(pItem);
			if (array) {
				return new plg::vector<T>(std::move(*array));
			}
			return nullptr;
		}

		void SetFallbackReturn(ValueType retType, const JitCallback::Return* ret) {
			switch (retType) {
			case ValueType::Void:
				break;
			case ValueType::Bool:
			case ValueType::Char8:
			case ValueType::Char16:
			case ValueType::Int8:
			case ValueType::Int16:
			case ValueType::Int32:
			case ValueType::Int64:
			case ValueType::UInt8:
			case ValueType::UInt16:
			case ValueType::UInt32:
			case ValueType::UInt64:
			case ValueType::Pointer:
			case ValueType::Float:
			case ValueType::Double:
				// HACK: Fill all 8 byte with 0
				ret->SetReturn<uintptr_t>({});
				break;
			case ValueType::Function:
				ret->SetReturn<void*>(nullptr);
				break;
			case ValueType::String:
				ret->ConstructAt<plg::string>();
				break;
			case ValueType::ArrayBool:
				ret->ConstructAt<plg::vector<bool>>();
				break;
			case ValueType::ArrayChar8:
				ret->ConstructAt<plg::vector<char>>();
				break;
			case ValueType::ArrayChar16:
				ret->ConstructAt<plg::vector<char16_t>>();
				break;
			case ValueType::ArrayInt8:
				ret->ConstructAt<plg::vector<int8_t>>();
				break;
			case ValueType::ArrayInt16:
				ret->ConstructAt<plg::vector<int16_t>>();
				break;
			case ValueType::ArrayInt32:
				ret->ConstructAt<plg::vector<int32_t>>();
				break;
			case ValueType::ArrayInt64:
				ret->ConstructAt<plg::vector<int64_t>>();
				break;
			case ValueType::ArrayUInt8:
				ret->ConstructAt<plg::vector<uint8_t>>();
				break;
			case ValueType::ArrayUInt16:
				ret->ConstructAt<plg::vector<uint16_t>>();
				break;
			case ValueType::ArrayUInt32:
				ret->ConstructAt<plg::vector<uint32_t>>();
				break;
			case ValueType::ArrayUInt64:
				ret->ConstructAt<plg::vector<uint64_t>>();
				break;
			case ValueType::ArrayPointer:
				ret->ConstructAt<plg::vector<void*>>();
				break;
			case ValueType::ArrayFloat:
				ret->ConstructAt<plg::vector<float>>();
				break;
			case ValueType::ArrayDouble:
				ret->ConstructAt<plg::vector<double>>();
				break;
			case ValueType::ArrayString:
				ret->ConstructAt<plg::vector<plg::string>>();
				break;
			case ValueType::Vector2:
				ret->SetReturn<Vector2>({});
				break;
			case ValueType::Vector3:
				ret->SetReturn<Vector3>({});
				break;
			case ValueType::Vector4:
				ret->SetReturn<Vector4>({});
				break;
			case ValueType::Matrix4x4:
				ret->SetReturn<Matrix4x4>({});
				break;
			default: {
				const std::string error(std::format("[py3lm] SetFallbackReturn unsupported type {:#x}", static_cast<uint8_t>(retType)));
				g_py3lm.LogFatal(error);
				std::terminate();
			}
			}
		}

		bool SetReturn(PyObject* result, PropertyRef retType, const JitCallback::Return* ret) {
			switch (retType.GetType()) {
			case ValueType::Void:
				return true;
			case ValueType::Bool:
				if (auto value = ValueFromObject<bool>(result)) {
					ret->SetReturn<bool>(*value);
					return true;
				}
				break;
			case ValueType::Char8:
				if (auto value = ValueFromObject<char>(result)) {
					ret->SetReturn<char>(*value);
					return true;
				}
				break;
			case ValueType::Char16:
				if (auto value = ValueFromObject<char16_t>(result)) {
					ret->SetReturn<char16_t>(*value);
					return true;
				}
				break;
			case ValueType::Int8:
				if (auto value = ValueFromObject<int8_t>(result)) {
					ret->SetReturn<int8_t>(*value);
					return true;
				}
				break;
			case ValueType::Int16:
				if (auto value = ValueFromObject<int16_t>(result)) {
					ret->SetReturn<int16_t>(*value);
					return true;
				}
				break;
			case ValueType::Int32:
				if (auto value = ValueFromObject<int32_t>(result)) {
					ret->SetReturn<int32_t>(*value);
					return true;
				}
				break;
			case ValueType::Int64:
				if (auto value = ValueFromObject<int64_t>(result)) {
					ret->SetReturn<int64_t>(*value);
					return true;
				}
				break;
			case ValueType::UInt8:
				if (auto value = ValueFromObject<uint8_t>(result)) {
					ret->SetReturn<uint8_t>(*value);
					return true;
				}
				break;
			case ValueType::UInt16:
				if (auto value = ValueFromObject<uint16_t>(result)) {
					ret->SetReturn<uint16_t>(*value);
					return true;
				}
				break;
			case ValueType::UInt32:
				if (auto value = ValueFromObject<uint32_t>(result)) {
					ret->SetReturn<uint32_t>(*value);
					return true;
				}
				break;
			case ValueType::UInt64:
				if (auto value = ValueFromObject<uint64_t>(result)) {
					ret->SetReturn<uint64_t>(*value);
					return true;
				}
				break;
			case ValueType::Pointer:
				if (auto value = ValueFromObject<void*>(result)) {
					ret->SetReturn<void*>(*value);
					return true;
				}
				break;
			case ValueType::Float:
				if (auto value = ValueFromObject<float>(result)) {
					ret->SetReturn<float>(*value);
					return true;
				}
				break;
			case ValueType::Double:
				if (auto value = ValueFromObject<double>(result)) {
					ret->SetReturn<double>(*value);
					return true;
				}
				break;
			case ValueType::Function:
				if (auto value = GetOrCreateFunctionValue(retType.GetPrototype().value(), result)) {
					ret->SetReturn<void*>(*value);
					return true;
				}
				break;
			case ValueType::String:
				if (auto value = ValueFromObject<plg::string>(result)) {
					ret->ConstructAt<plg::string>(std::move(*value));
					return true;
				}
				break;
			case ValueType::ArrayBool:
				if (auto value = ArrayFromObject<bool>(result)) {
					ret->ConstructAt<plg::vector<bool>>(std::move(*value));
					return true;
				}
				break;
			case ValueType::ArrayChar8:
				if (auto value = ArrayFromObject<char>(result)) {
					ret->ConstructAt<plg::vector<char>>(std::move(*value));
					return true;
				}
				break;
			case ValueType::ArrayChar16:
				if (auto value = ArrayFromObject<char16_t>(result)) {
					ret->ConstructAt<plg::vector<char16_t>>(std::move(*value));
					return true;
				}
				break;
			case ValueType::ArrayInt8:
				if (auto value = ArrayFromObject<int8_t>(result)) {
					ret->ConstructAt<plg::vector<int8_t>>(std::move(*value));
					return true;
				}
				break;
			case ValueType::ArrayInt16:
				if (auto value = ArrayFromObject<int16_t>(result)) {
					ret->ConstructAt<plg::vector<int16_t>>(std::move(*value));
					return true;
				}
				break;
			case ValueType::ArrayInt32:
				if (auto value = ArrayFromObject<int32_t>(result)) {
					ret->ConstructAt<plg::vector<int32_t>>(std::move(*value));
					return true;
				}
				break;
			case ValueType::ArrayInt64:
				if (auto value = ArrayFromObject<int64_t>(result)) {
					ret->ConstructAt<plg::vector<int64_t>>(std::move(*value));
					return true;
				}
				break;
			case ValueType::ArrayUInt8:
				if (auto value = ArrayFromObject<uint8_t>(result)) {
					ret->ConstructAt<plg::vector<uint8_t>>(std::move(*value));
					return true;
				}
				break;
			case ValueType::ArrayUInt16:
				if (auto value = ArrayFromObject<uint16_t>(result)) {
					ret->ConstructAt<plg::vector<uint16_t>>(std::move(*value));
					return true;
				}
				break;
			case ValueType::ArrayUInt32:
				if (auto value = ArrayFromObject<uint32_t>(result)) {
					ret->ConstructAt<plg::vector<uint32_t>>(std::move(*value));
					return true;
				}
				break;
			case ValueType::ArrayUInt64:
				if (auto value = ArrayFromObject<uint64_t>(result)) {
					ret->ConstructAt<plg::vector<uint64_t>>(std::move(*value));
					return true;
				}
				break;
			case ValueType::ArrayPointer:
				if (auto value = ArrayFromObject<void*>(result)) {
					ret->ConstructAt<plg::vector<void*>>(std::move(*value));
					return true;
				}
				break;
			case ValueType::ArrayFloat:
				if (auto value = ArrayFromObject<float>(result)) {
					ret->ConstructAt<plg::vector<float>>(std::move(*value));
					return true;
				}
				break;
			case ValueType::ArrayDouble:
				if (auto value = ArrayFromObject<double>(result)) {
					ret->ConstructAt<plg::vector<double>>(std::move(*value));
					return true;
				}
				break;
			case ValueType::ArrayString:
				if (auto value = ArrayFromObject<plg::string>(result)) {
					ret->ConstructAt<plg::vector<plg::string>>(std::move(*value));
					return true;
				}
				break;
			case ValueType::Vector2:
				if (auto value = ValueFromObject<Vector2>(result)) {
					ret->SetReturn<Vector2>(*value);
					return true;
				}
				break;
			case ValueType::Vector3:
				if (auto value = ValueFromObject<Vector3>(result)) {
					ret->SetReturn<Vector3>(*value);
					return true;
				}
				break;
			case ValueType::Vector4:
				if (auto value = ValueFromObject<Vector4>(result)) {
					ret->SetReturn<Vector4>(*value);
					return true;
				}
				break;
			case ValueType::Matrix4x4:
				if (auto value = ValueFromObject<Matrix4x4>(result)) {
					ret->SetReturn<Matrix4x4>(*value);
					return true;
				}
				break;
			default: {
				const std::string error(std::format("[py3lm] SetReturn unsupported type {:#x}", static_cast<uint8_t>(retType.GetType())));
				g_py3lm.LogFatal(error);
				std::terminate();
			}
			}

			return false;
		}

		bool SetRefParam(PyObject* object, PropertyRef paramType, const JitCallback::Parameters* params, uint8_t index) {
			switch (paramType.GetType()) {
			case ValueType::Bool:
				if (auto value = ValueFromObject<bool>(object)) {
					auto* const param = params->GetArgument<bool*>(index);
					*param = *value;
					return true;
				}
				break;
			case ValueType::Char8:
				if (auto value = ValueFromObject<char>(object)) {
					auto* const param = params->GetArgument<char*>(index);
					*param = *value;
					return true;
				}
				break;
			case ValueType::Char16:
				if (auto value = ValueFromObject<char16_t>(object)) {
					auto* const param = params->GetArgument<char16_t*>(index);
					*param = *value;
					return true;
				}
				break;
			case ValueType::Int8:
				if (auto value = ValueFromObject<int8_t>(object)) {
					auto* const param = params->GetArgument<int8_t*>(index);
					*param = *value;
					return true;
				}
				break;
			case ValueType::Int16:
				if (auto value = ValueFromObject<int16_t>(object)) {
					auto* const param = params->GetArgument<int16_t*>(index);
					*param = *value;
					return true;
				}
				break;
			case ValueType::Int32:
				if (auto value = ValueFromObject<int32_t>(object)) {
					auto* const param = params->GetArgument<int32_t*>(index);
					*param = *value;
					return true;
				}
				break;
			case ValueType::Int64:
				if (auto value = ValueFromObject<int64_t>(object)) {
					auto* const param = params->GetArgument<int64_t*>(index);
					*param = *value;
					return true;
				}
				break;
			case ValueType::UInt8:
				if (auto value = ValueFromObject<uint8_t>(object)) {
					auto* const param = params->GetArgument<uint8_t*>(index);
					*param = *value;
					return true;
				}
				break;
			case ValueType::UInt16:
				if (auto value = ValueFromObject<uint16_t>(object)) {
					auto* const param = params->GetArgument<uint16_t*>(index);
					*param = *value;
					return true;
				}
				break;
			case ValueType::UInt32:
				if (auto value = ValueFromObject<uint32_t>(object)) {
					auto* const param = params->GetArgument<uint32_t*>(index);
					*param = *value;
					return true;
				}
				break;
			case ValueType::UInt64:
				if (auto value = ValueFromObject<uint64_t>(object)) {
					auto* const param = params->GetArgument<uint64_t*>(index);
					*param = *value;
					return true;
				}
				break;
			case ValueType::Pointer:
				if (auto value = ValueFromObject<void*>(object)) {
					auto* const param = params->GetArgument<void**>(index);
					*param = *value;
					return true;
				}
				break;
			case ValueType::Float:
				if (auto value = ValueFromObject<float>(object)) {
					auto* const param = params->GetArgument<float*>(index);
					*param = *value;
					return true;
				}
				break;
			case ValueType::Double:
				if (auto value = ValueFromObject<double>(object)) {
					auto* const param = params->GetArgument<double*>(index);
					*param = *value;
					return true;
				}
				break;
			case ValueType::String:
				if (auto value = ValueFromObject<plg::string>(object)) {
					auto* const param = params->GetArgument<plg::string*>(index);
					*param = std::move(*value);
					return true;
				}
				break;
			case ValueType::ArrayBool:
				if (auto value = ArrayFromObject<bool>(object)) {
					auto* const param = params->GetArgument<plg::vector<bool>*>(index);
					*param = std::move(*value);
					return true;
				}
				break;
			case ValueType::ArrayChar8:
				if (auto value = ArrayFromObject<char>(object)) {
					auto* const param = params->GetArgument<plg::vector<char>*>(index);
					*param = std::move(*value);
					return true;
				}
				break;
			case ValueType::ArrayChar16:
				if (auto value = ArrayFromObject<char16_t>(object)) {
					auto* const param = params->GetArgument<plg::vector<char16_t>*>(index);
					*param = std::move(*value);
					return true;
				}
				break;
			case ValueType::ArrayInt8:
				if (auto value = ArrayFromObject<int8_t>(object)) {
					auto* const param = params->GetArgument<plg::vector<int8_t>*>(index);
					*param = std::move(*value);
					return true;
				}
				break;
			case ValueType::ArrayInt16:
				if (auto value = ArrayFromObject<int16_t>(object)) {
					auto* const param = params->GetArgument<plg::vector<int16_t>*>(index);
					*param = std::move(*value);
					return true;
				}
				break;
			case ValueType::ArrayInt32:
				if (auto value = ArrayFromObject<int32_t>(object)) {
					auto* const param = params->GetArgument<plg::vector<int32_t>*>(index);
					*param = std::move(*value);
					return true;
				}
				break;
			case ValueType::ArrayInt64:
				if (auto value = ArrayFromObject<int64_t>(object)) {
					auto* const param = params->GetArgument<plg::vector<int64_t>*>(index);
					*param = std::move(*value);
					return true;
				}
				break;
			case ValueType::ArrayUInt8:
				if (auto value = ArrayFromObject<uint8_t>(object)) {
					auto* const param = params->GetArgument<plg::vector<uint8_t>*>(index);
					*param = std::move(*value);
					return true;
				}
				break;
			case ValueType::ArrayUInt16:
				if (auto value = ArrayFromObject<uint16_t>(object)) {
					auto* const param = params->GetArgument<plg::vector<uint16_t>*>(index);
					*param = std::move(*value);
					return true;
				}
				break;
			case ValueType::ArrayUInt32:
				if (auto value = ArrayFromObject<uint32_t>(object)) {
					auto* const param = params->GetArgument<plg::vector<uint32_t>*>(index);
					*param = std::move(*value);
					return true;
				}
				break;
			case ValueType::ArrayUInt64:
				if (auto value = ArrayFromObject<uint64_t>(object)) {
					auto* const param = params->GetArgument<plg::vector<uint64_t>*>(index);
					*param = std::move(*value);
					return true;
				}
				break;
			case ValueType::ArrayPointer:
				if (auto value = ArrayFromObject<void*>(object)) {
					auto* const param = params->GetArgument<plg::vector<void*>*>(index);
					*param = std::move(*value);
					return true;
				}
				break;
			case ValueType::ArrayFloat:
				if (auto value = ArrayFromObject<float>(object)) {
					auto* const param = params->GetArgument<plg::vector<float>*>(index);
					*param = std::move(*value);
					return true;
				}
				break;
			case ValueType::ArrayDouble:
				if (auto value = ArrayFromObject<double>(object)) {
					auto* const param = params->GetArgument<plg::vector<double>*>(index);
					*param = std::move(*value);
					return true;
				}
				break;
			case ValueType::ArrayString:
				if (auto value = ArrayFromObject<plg::string>(object)) {
					auto* const param = params->GetArgument<plg::vector<plg::string>*>(index);
					*param = std::move(*value);
					return true;
				}
				break;
			case ValueType::Vector2:
				if (auto value = ValueFromObject<Vector2>(object)) {
					auto* const param = params->GetArgument<Vector2*>(index);
					*param = *value;
					return true;
				}
				break;
			case ValueType::Vector3:
				if (auto value = ValueFromObject<Vector3>(object)) {
					auto* const param = params->GetArgument<Vector3*>(index);
					*param = *value;
					return true;
				}
				break;
			case ValueType::Vector4:
				if (auto value = ValueFromObject<Vector4>(object)) {
					auto* const param = params->GetArgument<Vector4*>(index);
					*param = *value;
					return true;
				}
				break;
			case ValueType::Matrix4x4:
				if (auto value = ValueFromObject<Matrix4x4>(object)) {
					auto* const param = params->GetArgument<Matrix4x4*>(index);
					*param = *value;
					return true;
				}
				break;
			default: {
				const std::string error(std::format("[py3lm] SetRefParam unsupported type {:#x}", static_cast<uint8_t>(paramType.GetType())));
				g_py3lm.LogFatal(error);
				std::terminate();
			}
			}

			return false;
		}

		template<typename T>
		PyObject* CreatePyObject(T /*value*/) {
			static_assert(always_false_v<T>, "CreatePyObject specialization required");
			return nullptr;
		}

		template<>
		PyObject* CreatePyObject(bool value) {
			return PyBool_FromLong(value);
		}

		template<>
		PyObject* CreatePyObject(char value) {
			if (value == char{ 0 }) {
				return PyUnicode_FromStringAndSize(nullptr, Py_ssize_t{ 0 });
			}
			return PyUnicode_FromStringAndSize(&value, Py_ssize_t{ 1 });
		}

		template<>
		PyObject* CreatePyObject(char16_t value) {
			if (value == char16_t{ 0 }) {
				return PyUnicode_FromStringAndSize(nullptr, Py_ssize_t{ 0 });
			}
			const auto [rc, out] = ConvertUtf16ToUtf8(value);
			if (rc == -1) {
				PyErr_SetString(PyExc_ValueError, "Surrogate pair");
				return nullptr;
			}
			return PyUnicode_FromStringAndSize(out.data(), static_cast<Py_ssize_t>(rc));
		}

		template<>
		PyObject* CreatePyObject(int8_t value) {
			return PyLong_FromLong(value);
		}

		template<>
		PyObject* CreatePyObject(int16_t value) {
			return PyLong_FromLong(value);
		}

		template<>
		PyObject* CreatePyObject(int32_t value) {
			return PyLong_FromLong(value);
		}

		template<>
		PyObject* CreatePyObject(int64_t value) {
			return PyLong_FromLongLong(value);
		}

		template<>
		PyObject* CreatePyObject(uint8_t value) {
			return PyLong_FromUnsignedLong(value);
		}

		template<>
		PyObject* CreatePyObject(uint16_t value) {
			return PyLong_FromUnsignedLong(value);
		}

		template<>
		PyObject* CreatePyObject(uint32_t value) {
			return PyLong_FromUnsignedLong(value);
		}

		template<>
		PyObject* CreatePyObject(uint64_t value) {
			return PyLong_FromUnsignedLongLong(value);
		}

		template<>
		PyObject* CreatePyObject(void* value) {
			return PyLong_FromVoidPtr(value);
		}

		template<>
		PyObject* CreatePyObject(float value) {
			return PyFloat_FromDouble(static_cast<double>(value));
		}

		template<>
		PyObject* CreatePyObject(double value) {
			return PyFloat_FromDouble(value);
		}

		template<>
		PyObject* CreatePyObject(plg::string value) {
			return PyUnicode_FromStringAndSize(value.data(), static_cast<Py_ssize_t>(value.size()));
		}

		template<>
		PyObject* CreatePyObject(Vector2 value) {
			return g_py3lm.CreateVector2Object(value);
		}

		template<>
		PyObject* CreatePyObject(Vector3 value) {
			return g_py3lm.CreateVector3Object(value);
		}

		template<>
		PyObject* CreatePyObject(Vector4 value) {
			return g_py3lm.CreateVector4Object(value);
		}

		template<>
		PyObject* CreatePyObject(Matrix4x4 value) {
			return g_py3lm.CreateMatrix4x4Object(value);
		}

		PyObject* GetOrCreateFunctionObject(MethodRef method, void* funcAddr) {
			return g_py3lm.GetOrCreateFunctionObject(method, funcAddr);
		}

		template<typename T>
		PyObject* CreatePyObjectList(const plg::vector<T>& arrayArg) {
			const auto size = static_cast<Py_ssize_t>(arrayArg.size());
			PyObject* const arrayObject = PyList_New(size);
			if (arrayObject) {
				for (Py_ssize_t i = 0; i < size; ++i) {
					PyObject* const valueObject = CreatePyObject(arrayArg[i]);
					if (!valueObject) {
						Py_DECREF(arrayObject);
						return nullptr;
					}
					PyList_SET_ITEM(arrayObject, i, valueObject);
				}
			}
			return arrayObject;
		}

		PyObject* ParamToObject(PropertyRef paramType, const JitCallback::Parameters* params, uint8_t index) {
			switch (paramType.GetType()) {
			case ValueType::Bool:
				return CreatePyObject(params->GetArgument<bool>(index));
			case ValueType::Char8:
				return CreatePyObject(params->GetArgument<char>(index));
			case ValueType::Char16:
				return CreatePyObject(params->GetArgument<char16_t>(index));
			case ValueType::Int8:
				return CreatePyObject(params->GetArgument<int8_t>(index));
			case ValueType::Int16:
				return CreatePyObject(params->GetArgument<int16_t>(index));
			case ValueType::Int32:
				return CreatePyObject(params->GetArgument<int32_t>(index));
			case ValueType::Int64:
				return CreatePyObject(params->GetArgument<int64_t>(index));
			case ValueType::UInt8:
				return CreatePyObject(params->GetArgument<uint8_t>(index));
			case ValueType::UInt16:
				return CreatePyObject(params->GetArgument<uint16_t>(index));
			case ValueType::UInt32:
				return CreatePyObject(params->GetArgument<uint32_t>(index));
			case ValueType::UInt64:
				return CreatePyObject(params->GetArgument<uint64_t>(index));
			case ValueType::Pointer:
				return CreatePyObject(params->GetArgument<void*>(index));
			case ValueType::Float:
				return CreatePyObject(params->GetArgument<float>(index));
			case ValueType::Double:
				return CreatePyObject(params->GetArgument<double>(index));
			case ValueType::Function:
				return GetOrCreateFunctionObject(paramType.GetPrototype().value(), params->GetArgument<void*>(index));
			case ValueType::String:
				return CreatePyObject(*(params->GetArgument<const plg::string*>(index)));
			case ValueType::ArrayBool:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<bool>*>(index)));
			case ValueType::ArrayChar8:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<char>*>(index)));
			case ValueType::ArrayChar16:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<char16_t>*>(index)));
			case ValueType::ArrayInt8:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<int8_t>*>(index)));
			case ValueType::ArrayInt16:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<int16_t>*>(index)));
			case ValueType::ArrayInt32:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<int32_t>*>(index)));
			case ValueType::ArrayInt64:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<int64_t>*>(index)));
			case ValueType::ArrayUInt8:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<uint8_t>*>(index)));
			case ValueType::ArrayUInt16:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<uint16_t>*>(index)));
			case ValueType::ArrayUInt32:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<uint32_t>*>(index)));
			case ValueType::ArrayUInt64:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<uint64_t>*>(index)));
			case ValueType::ArrayPointer:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<void*>*>(index)));
			case ValueType::ArrayFloat:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<float>*>(index)));
			case ValueType::ArrayDouble:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<double>*>(index)));
			case ValueType::ArrayString:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<plg::string>*>(index)));
			case ValueType::Vector2:
				return CreatePyObject(*(params->GetArgument<Vector2*>(index)));
			case ValueType::Vector3:
				return CreatePyObject(*(params->GetArgument<Vector3*>(index)));
			case ValueType::Vector4:
				return CreatePyObject(*(params->GetArgument<Vector4*>(index)));
			case ValueType::Matrix4x4:
				return CreatePyObject(*(params->GetArgument<Matrix4x4*>(index)));
			default: {
				const std::string error(std::format("[py3lm] ParamToObject unsupported type {:#x}", static_cast<uint8_t>(paramType.GetType())));
				g_py3lm.LogFatal(error);
				std::terminate();
				return nullptr;
			}
			}
		}

		PyObject* ParamRefToObject(PropertyRef paramType, const JitCallback::Parameters* params, uint8_t index) {
			switch (paramType.GetType()) {
			case ValueType::Bool:
				return CreatePyObject(*(params->GetArgument<bool*>(index)));
			case ValueType::Char8:
				return CreatePyObject(*(params->GetArgument<char*>(index)));
			case ValueType::Char16:
				return CreatePyObject(*(params->GetArgument<char16_t*>(index)));
			case ValueType::Int8:
				return CreatePyObject(*(params->GetArgument<int8_t*>(index)));
			case ValueType::Int16:
				return CreatePyObject(*(params->GetArgument<int16_t*>(index)));
			case ValueType::Int32:
				return CreatePyObject(*(params->GetArgument<int32_t*>(index)));
			case ValueType::Int64:
				return CreatePyObject(*(params->GetArgument<int64_t*>(index)));
			case ValueType::UInt8:
				return CreatePyObject(*(params->GetArgument<uint8_t*>(index)));
			case ValueType::UInt16:
				return CreatePyObject(*(params->GetArgument<uint16_t*>(index)));
			case ValueType::UInt32:
				return CreatePyObject(*(params->GetArgument<uint32_t*>(index)));
			case ValueType::UInt64:
				return CreatePyObject(*(params->GetArgument<uint64_t*>(index)));
			case ValueType::Pointer:
				return CreatePyObject(*(params->GetArgument<void**>(index)));
			case ValueType::Float:
				return CreatePyObject(*(params->GetArgument<float*>(index)));
			case ValueType::Double:
				return CreatePyObject(*(params->GetArgument<double*>(index)));
			case ValueType::String:
				return CreatePyObject(*(params->GetArgument<const plg::string*>(index)));
			case ValueType::ArrayBool:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<bool>*>(index)));
			case ValueType::ArrayChar8:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<char>*>(index)));
			case ValueType::ArrayChar16:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<char16_t>*>(index)));
			case ValueType::ArrayInt8:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<int8_t>*>(index)));
			case ValueType::ArrayInt16:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<int16_t>*>(index)));
			case ValueType::ArrayInt32:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<int32_t>*>(index)));
			case ValueType::ArrayInt64:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<int64_t>*>(index)));
			case ValueType::ArrayUInt8:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<uint8_t>*>(index)));
			case ValueType::ArrayUInt16:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<uint16_t>*>(index)));
			case ValueType::ArrayUInt32:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<uint32_t>*>(index)));
			case ValueType::ArrayUInt64:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<uint64_t>*>(index)));
			case ValueType::ArrayPointer:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<void*>*>(index)));
			case ValueType::ArrayFloat:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<float>*>(index)));
			case ValueType::ArrayDouble:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<double>*>(index)));
			case ValueType::ArrayString:
				return CreatePyObjectList(*(params->GetArgument<const plg::vector<plg::string>*>(index)));
			case ValueType::Vector2:
				return CreatePyObject(*(params->GetArgument<Vector2*>(index)));
			case ValueType::Vector3:
				return CreatePyObject(*(params->GetArgument<Vector3*>(index)));
			case ValueType::Vector4:
				return CreatePyObject(*(params->GetArgument<Vector4*>(index)));
			case ValueType::Matrix4x4:
				return CreatePyObject(*(params->GetArgument<Matrix4x4*>(index)));
			default: {
				const std::string error(std::format("[py3lm] ParamRefToObject unsupported type {:#x}", static_cast<uint8_t>(paramType.GetType())));
				g_py3lm.LogFatal(error);
				std::terminate();
				return nullptr;
			}
			}
		}

		void InternalCall(MethodRef method, MemAddr data, const JitCallback::Parameters* params, const uint8_t count, const JitCallback::Return* ret) {
			PyObject* const func = data.RCast<PyObject*>();

			enum class ParamProcess {
				NoError,
				Error,
				ErrorWithException
			};
			ParamProcess processResult = ParamProcess::NoError;

			const auto paramTypes = method.GetParamTypes();
			uint8_t paramsCount = static_cast<uint8_t>(paramTypes.size());
			uint8_t refParamsCount = 0;

			PyObject* argTuple = nullptr;
			if (paramsCount) {
				argTuple = PyTuple_New(paramsCount);
				if (!argTuple) {
					processResult = ParamProcess::ErrorWithException;
					PyErr_SetString(PyExc_RuntimeError, "Fail to create arguments tuple");
				}
				else {
					for (uint8_t index = 0; index < paramsCount; ++index) {
						const PropertyRef paramType = paramTypes[index];
						if (paramType.IsReference()) {
							++refParamsCount;
						}
						using ParamConvertionFunc = PyObject* (*)(PropertyRef, const JitCallback::Parameters*, uint8_t);
						ParamConvertionFunc const convertFunc = paramType.IsReference() ? &ParamRefToObject : &ParamToObject;
						PyObject* const arg = convertFunc(paramType, params, index);
						if (!arg) {
							// convertFunc may set error
							processResult = PyErr_Occurred() ? ParamProcess::ErrorWithException : ParamProcess::Error;
							break;
						}
						if (PyTuple_SetItem(argTuple, index, arg) != 0) {
							Py_DECREF(arg);
							// PyTuple_SetItem set error
							processResult = ParamProcess::ErrorWithException;
							break;
						}
						// arg reference "stolen" by tuple set, no need to handle
					}
				}
			}

			if (processResult != ParamProcess::NoError) {
				if (argTuple) {
					Py_DECREF(argTuple);
				}
				if (processResult == ParamProcess::ErrorWithException) {
					g_py3lm.LogError();
				}

				SetFallbackReturn(method.GetReturnType().GetType(), ret);

				return;
			}

			const bool hasRefParams = refParamsCount != 0;

			PyObject* const result = PyObject_CallObject(func, argTuple);

			if (argTuple) {
				Py_DECREF(argTuple);
			}

			if (!result) {
				g_py3lm.LogError();

				SetFallbackReturn(method.GetReturnType().GetType(), ret);

				return;
			}

			if (hasRefParams) {
				if (!PyTuple_CheckExact(result)) {
					PyErr_SetString(PyExc_TypeError, "Returned value not tuple");

					g_py3lm.LogError();

					Py_DECREF(result);

					SetFallbackReturn(method.GetReturnType().GetType(), ret);

					return;
				}
				const Py_ssize_t tupleSize = PyTuple_Size(result);
				if (tupleSize != static_cast<Py_ssize_t>(1 + refParamsCount)) {
					const std::string error(std::format("Returned tuple wrong size {}, expected {}", tupleSize, static_cast<Py_ssize_t>(1 + refParamsCount)));
					PyErr_SetString(PyExc_TypeError, error.c_str());

					g_py3lm.LogError();

					Py_DECREF(result);

					SetFallbackReturn(method.GetReturnType().GetType(), ret);

					return;
				}
			}

			PyObject* const returnObject = hasRefParams ? PyTuple_GET_ITEM(result, Py_ssize_t{ 0 }) : result;

			if (hasRefParams) {
				for (uint8_t index = 0, k = 0; index < paramsCount; ++index) {
					const PropertyRef paramType = paramTypes[index];
					if (!paramType.IsReference()) {
						continue;
					}
					if (!SetRefParam(PyTuple_GET_ITEM(result, Py_ssize_t{ 1 + k }), paramType, params, index)) {
						// SetRefParam may set error
						if (PyErr_Occurred()) {
							g_py3lm.LogError();
						}
					}
					++k;
					if (k == refParamsCount) {
						break;
					}
				}
			}

			if (!SetReturn(returnObject, method.GetReturnType(), ret)) {
				if (PyErr_Occurred()) {
					g_py3lm.LogError();
				}

				SetFallbackReturn(method.GetReturnType().GetType(), ret);
			}

			Py_DECREF(result);
		}

		std::tuple<bool, JitCallback> CreateInternalCall(const std::shared_ptr<asmjit::JitRuntime>& jitRuntime, MethodRef method, PyObject* func) {
			JitCallback callback(jitRuntime);
			void* const methodAddr = callback.GetJitFunc(method, &InternalCall, reinterpret_cast<void*>(func));
			return { methodAddr != nullptr, std::move(callback) };
		}

		MethodExportResult GenerateMethodExport(MethodRef method, const std::shared_ptr<asmjit::JitRuntime>& jitRuntime, PyObject* pluginModule, PyObject* pluginInstance) {
			PyObject* func{};

			std::string className, methodName;
			{
				const auto& funcName = method.GetFunctionName();
				if (const auto pos = funcName.find('.'); pos != std::string::npos) {
					className = funcName.substr(0, pos);
					methodName = std::string(funcName.begin() + (pos + 1), funcName.end());
				}
				else {
					methodName = funcName;
				}
			}

			const bool funcIsMethod = !className.empty();

			if (funcIsMethod) {
				PyObject* const classType = PyObject_GetAttrString(pluginModule, className.c_str());
				if (classType) {
					func = PyObject_GetAttrString(classType, methodName.c_str());
					Py_DECREF(classType);
				}
			}
			else {
				func = PyObject_GetAttrString(pluginModule, methodName.c_str());
			}

			if (!func) {
				PyErr_Clear();
				return MethodExportError{ std::format("{} (Not found '{}' in module)", method.GetName(), method.GetFunctionName())};
			}

			if (!PyFunction_Check(func) && !PyCallable_Check(func)) {
				Py_DECREF(func);
				return MethodExportError{ std::format("{} ('{}' not function type)", method.GetName(), method.GetFunctionName()) };
			}

			if (funcIsMethod && !IsStaticMethod(func)) {
				PyObject* const bind = PyMethod_New(func, pluginInstance);
				Py_DECREF(func);
				if (!bind) {
					return MethodExportError{ std::format("{} (instance bind fail)", method.GetName()) };
				}
				func = bind;
			}

			auto [result, callback] = CreateInternalCall(jitRuntime, method, func);

			if (!result) {
				Py_DECREF(func);
				return MethodExportError{ std::format("{} (jit error: {})", method.GetName(), callback.GetError()) };
			}

			return MethodExportData{ std::move(callback), func };
		}

		struct ArgsScope {
			JitCall::Parameters params;
			std::vector<std::pair<void*, ValueType>> storage; // used to store array temp memory

			explicit ArgsScope(uint8_t size) : params(size) {
				storage.reserve(size);
			}

			~ArgsScope() {
				for (auto& [ptr, type] : storage) {
					switch (type) {
					case ValueType::Bool: {
						delete reinterpret_cast<bool*>(ptr);
						break;
					}
					case ValueType::Char8: {
						delete reinterpret_cast<char*>(ptr);
						break;
					}
					case ValueType::Char16: {
						delete reinterpret_cast<char16_t*>(ptr);
						break;
					}
					case ValueType::Int8: {
						delete reinterpret_cast<int8_t*>(ptr);
						break;
					}
					case ValueType::Int16: {
						delete reinterpret_cast<int16_t*>(ptr);
						break;
					}
					case ValueType::Int32: {
						delete reinterpret_cast<int32_t*>(ptr);
						break;
					}
					case ValueType::Int64: {
						delete reinterpret_cast<int64_t*>(ptr);
						break;
					}
					case ValueType::UInt8: {
						delete reinterpret_cast<uint8_t*>(ptr);
						break;
					}
					case ValueType::UInt16: {
						delete reinterpret_cast<uint16_t*>(ptr);
						break;
					}
					case ValueType::UInt32: {
						delete reinterpret_cast<uint32_t*>(ptr);
						break;
					}
					case ValueType::UInt64: {
						delete reinterpret_cast<uint64_t*>(ptr);
						break;
					}
					case ValueType::Pointer: {
						delete reinterpret_cast<void**>(ptr);
						break;
					}
					case ValueType::Float: {
						delete reinterpret_cast<float*>(ptr);
						break;
					}
					case ValueType::Double: {
						delete reinterpret_cast<double*>(ptr);
						break;
					}
					case ValueType::String: {
						delete reinterpret_cast<plg::string*>(ptr);
						break;
					}
					case ValueType::ArrayBool: {
						delete reinterpret_cast<plg::vector<bool>*>(ptr);
						break;
					}
					case ValueType::ArrayChar8: {
						delete reinterpret_cast<plg::vector<char>*>(ptr);
						break;
					}
					case ValueType::ArrayChar16: {
						delete reinterpret_cast<plg::vector<char16_t>*>(ptr);
						break;
					}
					case ValueType::ArrayInt8: {
						delete reinterpret_cast<plg::vector<int16_t>*>(ptr);
						break;
					}
					case ValueType::ArrayInt16: {
						delete reinterpret_cast<plg::vector<int16_t>*>(ptr);
						break;
					}
					case ValueType::ArrayInt32: {
						delete reinterpret_cast<plg::vector<int32_t>*>(ptr);
						break;
					}
					case ValueType::ArrayInt64: {
						delete reinterpret_cast<plg::vector<int64_t>*>(ptr);
						break;
					}
					case ValueType::ArrayUInt8: {
						delete reinterpret_cast<plg::vector<uint8_t>*>(ptr);
						break;
					}
					case ValueType::ArrayUInt16: {
						delete reinterpret_cast<plg::vector<uint16_t>*>(ptr);
						break;
					}
					case ValueType::ArrayUInt32: {
						delete reinterpret_cast<plg::vector<uint32_t>*>(ptr);
						break;
					}
					case ValueType::ArrayUInt64: {
						delete reinterpret_cast<plg::vector<uint64_t>*>(ptr);
						break;
					}
					case ValueType::ArrayPointer: {
						delete reinterpret_cast<plg::vector<void*>*>(ptr);
						break;
					}
					case ValueType::ArrayFloat: {
						delete reinterpret_cast<plg::vector<float>*>(ptr);
						break;
					}
					case ValueType::ArrayDouble: {
						delete reinterpret_cast<plg::vector<double>*>(ptr);
						break;
					}
					case ValueType::ArrayString: {
						delete reinterpret_cast<plg::vector<plg::string>*>(ptr);
						break;
					}
					case ValueType::Vector2: {
						delete reinterpret_cast<Vector2*>(ptr);
						break;
					}
					case ValueType::Vector3: {
						delete reinterpret_cast<Vector3*>(ptr);
						break;
					}
					case ValueType::Vector4: {
						delete reinterpret_cast<Vector4*>(ptr);
						break;
					}
					case ValueType::Matrix4x4: {
						delete reinterpret_cast<Matrix4x4*>(ptr);
						break;
					}
					default: {
						const std::string error(std::format("[py3lm] ArgsScope unhandled type {:#x}", static_cast<uint8_t>(type)));
						g_py3lm.LogFatal(error);
						std::terminate();
						break;
					}
					}
				}
			}
		};

		void BeginExternalCall(MethodRef method, ArgsScope& a) {
			ValueType retType = method.GetReturnType().GetType();
			if (ValueUtils::IsHiddenParam(retType)) {
				void* value;
				switch (retType) {
					case ValueType::String: {
						value = new plg::string();
						a.storage.emplace_back(value, retType);
						break;
					}
					case ValueType::ArrayBool: {
						value = new plg::vector<bool>();
						a.storage.emplace_back(value, retType);
						break;
					}
					case ValueType::ArrayChar8: {
						value = new plg::vector<char>();
						a.storage.emplace_back(value, retType);
						break;
					}
					case ValueType::ArrayChar16: {
						value = new plg::vector<char16_t>();
						a.storage.emplace_back(value, retType);
						break;
					}
					case ValueType::ArrayInt8: {
						value = new plg::vector<int8_t>();
						a.storage.emplace_back(value, retType);
						break;
					}
					case ValueType::ArrayInt16: {
						value = new plg::vector<int16_t>();
						a.storage.emplace_back(value, retType);
						break;
					}
					case ValueType::ArrayInt32: {
						value = new plg::vector<int32_t>();
						a.storage.emplace_back(value, retType);
						break;
					}
					case ValueType::ArrayInt64: {
						value = new plg::vector<int64_t>();
						a.storage.emplace_back(value, retType);
						break;
					}
					case ValueType::ArrayUInt8: {
						value = new plg::vector<uint8_t>();
						a.storage.emplace_back(value, retType);
						break;
					}
					case ValueType::ArrayUInt16: {
						value = new plg::vector<uint16_t>();
						a.storage.emplace_back(value, retType);
						break;
					}
					case ValueType::ArrayUInt32: {
						value = new plg::vector<uint32_t>();
						a.storage.emplace_back(value, retType);
						break;
					}
					case ValueType::ArrayUInt64: {
						value = new plg::vector<uint64_t>();
						a.storage.emplace_back(value, retType);
						break;
					}
					case ValueType::ArrayPointer: {
						value = new plg::vector<void*>();
						a.storage.emplace_back(value, retType);
						break;
					}
					case ValueType::ArrayFloat: {
						value = new plg::vector<float>();
						a.storage.emplace_back(value, retType);
						break;
					}
					case ValueType::ArrayDouble: {
						value = new plg::vector<double>();
						a.storage.emplace_back(value, retType);
						break;
					}
					case ValueType::ArrayString: {
						value = new plg::vector<plg::string>();
						a.storage.emplace_back(value, retType);
						break;
					}
					case ValueType::Vector2: {
						value = new plugify::Vector2();
						a.storage.emplace_back(value, retType);
						break;
					}
					case ValueType::Vector3: {
						value = new plugify::Vector3();
						a.storage.emplace_back(value, retType);
						break;
					}
					case ValueType::Vector4: {
						value = new plugify::Vector4();
						a.storage.emplace_back(value, retType);
						break;
					}
					case ValueType::Matrix4x4: {
						value = new plugify::Matrix4x4();
						a.storage.emplace_back(value, retType);
						break;
					}
					default:
						const std::string error(std::format("[py3lm] BeginExternalCall unsupported type {:#x}", static_cast<uint8_t>(retType)));
						g_py3lm.LogFatal(error);
						std::terminate();
						break;
				}

				a.params.AddArgument(value);
			}
		}

		PyObject* MakeExternalCall(MethodRef method, JitCall::CallingFunc func, const ArgsScope& a, JitCall::Return& ret) {
			func(a.params.GetDataPtr(), &ret);
			ValueType retType = method.GetReturnType().GetType();
			switch (retType) {
			case ValueType::Void:
				return Py_None;
			case ValueType::Bool: {
				const bool val = ret.GetReturn<bool>();
				return CreatePyObject(val);
			}
			case ValueType::Char8: {
				const char val = ret.GetReturn<char>();
				return CreatePyObject(val);
			}
			case ValueType::Char16: {
				const char16_t val = ret.GetReturn<char16_t>();
				return CreatePyObject(val);
			}
			case ValueType::Int8: {
				const int8_t val = ret.GetReturn<int8_t>();
				return CreatePyObject(val);
			}
			case ValueType::Int16: {
				const int16_t val = ret.GetReturn<int16_t>();
				return CreatePyObject(val);
			}
			case ValueType::Int32: {
				const int32_t val = ret.GetReturn<int32_t>();
				return CreatePyObject(val);
			}
			case ValueType::Int64: {
				const int64_t val = ret.GetReturn<int64_t>();
				return CreatePyObject(val);
			}
			case ValueType::UInt8: {
				const uint8_t val = ret.GetReturn<uint8_t>();
				return CreatePyObject(val);
			}
			case ValueType::UInt16: {
				const uint16_t val = ret.GetReturn<uint16_t>();
				return CreatePyObject(val);
			}
			case ValueType::UInt32: {
				const uint32_t val = ret.GetReturn<uint32_t>();
				return CreatePyObject(val);
			}
			case ValueType::UInt64: {
				const uint64_t val = ret.GetReturn<uint64_t>();
				return CreatePyObject(val);
			}
			case ValueType::Pointer: {
				void* val = ret.GetReturn<void*>();
				return CreatePyObject(val);
			}
			case ValueType::Float: {
				const float val = ret.GetReturn<float>();
				return CreatePyObject(val);
			}
			case ValueType::Double: {
				const double val = ret.GetReturn<double>();
				return CreatePyObject(val);
			}
			case ValueType::Function: {
				void* const val = ret.GetReturn<void*>();
				return GetOrCreateFunctionObject(method.GetReturnType().GetPrototype().value(), val);
			}
			case ValueType::String: {
				auto* const str = ret.GetReturn<plg::string*>();
				return CreatePyObject(*str);
			}
			case ValueType::ArrayBool: {
				auto* const arr = ret.GetReturn<plg::vector<bool>*>();
				return CreatePyObjectList<bool>(*arr);
			}
			case ValueType::ArrayChar8: {
				auto* const arr = ret.GetReturn<plg::vector<char>*>();
				return CreatePyObjectList<char>(*arr);
			}
			case ValueType::ArrayChar16: {
				auto* const arr = ret.GetReturn<plg::vector<char16_t>*>();
				return CreatePyObjectList<char16_t>(*arr);
			}
			case ValueType::ArrayInt8: {
				auto* const arr = ret.GetReturn<plg::vector<int8_t>*>();
				return CreatePyObjectList<int8_t>(*arr);
			}
			case ValueType::ArrayInt16: {
				auto* const arr = ret.GetReturn<plg::vector<int16_t>*>();
				return CreatePyObjectList<int16_t>(*arr);
			}
			case ValueType::ArrayInt32: {
				auto* const arr = ret.GetReturn<plg::vector<int32_t>*>();
				return CreatePyObjectList<int32_t>(*arr);
			}
			case ValueType::ArrayInt64: {
				auto* const arr = ret.GetReturn<plg::vector<int64_t>*>();
				return CreatePyObjectList<int64_t>(*arr);
			}
			case ValueType::ArrayUInt8: {
				auto* const arr = ret.GetReturn<plg::vector<uint8_t>*>();
				return CreatePyObjectList<uint8_t>(*arr);
			}
			case ValueType::ArrayUInt16: {
				auto* const arr = ret.GetReturn<plg::vector<uint16_t>*>();
				return CreatePyObjectList<uint16_t>(*arr);
			}
			case ValueType::ArrayUInt32: {
				auto* const arr = ret.GetReturn<plg::vector<uint32_t>*>();
				return CreatePyObjectList<uint32_t>(*arr);
			}
			case ValueType::ArrayUInt64: {
				auto* const arr = ret.GetReturn<plg::vector<uint64_t>*>();
				return CreatePyObjectList<uint64_t>(*arr);
			}
			case ValueType::ArrayPointer: {
				auto* const arr = ret.GetReturn<plg::vector<void*>*>();
				return CreatePyObjectList<void*>(*arr);
			}
			case ValueType::ArrayFloat: {
				auto* const arr = ret.GetReturn<plg::vector<float>*>();
				return CreatePyObjectList<float>(*arr);
			}
			case ValueType::ArrayDouble: {
				auto* const arr = ret.GetReturn<plg::vector<double>*>();
				return CreatePyObjectList<double>(*arr);
			}
			case ValueType::ArrayString: {
				auto* const arr = ret.GetReturn<plg::vector<plg::string>*>();
				return CreatePyObjectList<plg::string>(*arr);
			}
			case ValueType::Vector2: {
				const Vector2 val = ret.GetReturn<Vector2>();
				return CreatePyObject(val);
			}
			case ValueType::Vector3: {
				Vector3 val;
				if (ValueUtils::IsHiddenParam(retType)) {
					val = *ret.GetReturn<Vector3*>();
				} else {
					val = ret.GetReturn<Vector3>();
				}
				return CreatePyObject(val);
			}
			case ValueType::Vector4: {
				Vector4 val;
				if (ValueUtils::IsHiddenParam(retType)) {
					val = *ret.GetReturn<Vector4*>();
				} else {
					val = ret.GetReturn<Vector4>();
				}
				return CreatePyObject(val);
			}
			case ValueType::Matrix4x4: {
				Matrix4x4 val = *ret.GetReturn<Matrix4x4*>();
				return CreatePyObject(val);
			}
			default: {
				const std::string error(std::format("MakeExternalCall unsupported type {:#x}", static_cast<uint8_t>(retType)));
				PyErr_SetString(PyExc_RuntimeError, error.c_str());
				return nullptr;
			}
			}

			return nullptr;
		}

		bool PushObjectAsParam(PropertyRef paramType, PyObject* pItem, ArgsScope& a) {
			switch (paramType.GetType()) {
			case ValueType::Bool: {
				const auto value = ValueFromObject<bool>(pItem);
				if (!value) {
					return false;
				}
				a.params.AddArgument(*value);
				return true;
			}
			case ValueType::Char8: {
				const auto value = ValueFromObject<char>(pItem);
				if (!value) {
					return false;
				}
				a.params.AddArgument(*value);
				return true;
			}
			case ValueType::Char16: {
				const auto value = ValueFromObject<char16_t>(pItem);
				if (!value) {
					return false;
				}
				a.params.AddArgument(static_cast<short>(*value));
				return true;
			}
			case ValueType::Int8: {
				const auto value = ValueFromObject<int8_t>(pItem);
				if (!value) {
					return false;
				}
				a.params.AddArgument(*value);
				return true;
			}
			case ValueType::Int16: {
				const auto value = ValueFromObject<int16_t>(pItem);
				if (!value) {
					return false;
				}
				a.params.AddArgument(*value);
				return true;
			}
			case ValueType::Int32: {
				const auto value = ValueFromObject<int32_t>(pItem);
				if (!value) {
					return false;
				}
				a.params.AddArgument(*value);
				return true;
			}
			case ValueType::Int64: {
				const auto value = ValueFromObject<int64_t>(pItem);
				if (!value) {
					return false;
				}
				a.params.AddArgument(*value);
				return true;
			}
			case ValueType::UInt8: {
				const auto value = ValueFromObject<uint8_t>(pItem);
				if (!value) {
					return false;
				}
				a.params.AddArgument(static_cast<int8_t>(*value));
				return true;
			}
			case ValueType::UInt16: {
				const auto value = ValueFromObject<uint16_t>(pItem);
				if (!value) {
					return false;
				}
				a.params.AddArgument(static_cast<int16_t>(*value));
				return true;
			}
			case ValueType::UInt32: {
				const auto value = ValueFromObject<uint32_t>(pItem);
				if (!value) {
					return false;
				}
				a.params.AddArgument(static_cast<int32_t>(*value));
				return true;
			}
			case ValueType::UInt64: {
				const auto value = ValueFromObject<uint64_t>(pItem);
				if (!value) {
					return false;
				}
				a.params.AddArgument(static_cast<int64_t>(*value));
				return true;
			}
			case ValueType::Pointer: {
				const auto value = ValueFromObject<void*>(pItem);
				if (!value) {
					return false;
				}
				a.params.AddArgument(reinterpret_cast<void*>(*value));
				return true;
			}
			case ValueType::Float: {
				const auto value = ValueFromObject<float>(pItem);
				if (!value) {
					return false;
				}
				a.params.AddArgument(*value);
				return true;
			}
			case ValueType::Double: {
				const auto value = ValueFromObject<double>(pItem);
				if (!value) {
					return false;
				}
				a.params.AddArgument(*value);
				return true;
			}
			case ValueType::String: {
				void* const value = CreateValue<plg::string>(pItem);
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			}
			case ValueType::Function: {
				const auto value = GetOrCreateFunctionValue(paramType.GetPrototype().value(), pItem);
				if (!value) {
					return false;
				}
				a.params.AddArgument(reinterpret_cast<void*>(*value));
				return true;
			}
			case ValueType::ArrayBool: {
				void* const value = CreateArray<bool>(pItem);
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			}
			case ValueType::ArrayChar8: {
				void* const value = CreateArray<char>(pItem);
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			}
			case ValueType::ArrayChar16: {
				void* const value = CreateArray<char16_t>(pItem);
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			}
			case ValueType::ArrayInt8: {
				void* const value = CreateArray<int8_t>(pItem);
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			}
			case ValueType::ArrayInt16: {
				void* const value = CreateArray<int16_t>(pItem);
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			}
			case ValueType::ArrayInt32: {
				void* const value = CreateArray<int32_t>(pItem);
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			}
			case ValueType::ArrayInt64: {
				void* const value = CreateArray<int64_t>(pItem);
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			}
			case ValueType::ArrayUInt8: {
				void* const value = CreateArray<uint8_t>(pItem);
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			}
			case ValueType::ArrayUInt16: {
				void* const value = CreateArray<uint16_t>(pItem);
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			}
			case ValueType::ArrayUInt32: {
				void* const value = CreateArray<uint32_t>(pItem);
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			}
			case ValueType::ArrayUInt64: {
				void* const value = CreateArray<uint64_t>(pItem);
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			}
			case ValueType::ArrayPointer: {
				void* const value = CreateArray<void*>(pItem);
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			}
			case ValueType::ArrayFloat: {
				void* const value = CreateArray<float>(pItem);
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			}
			case ValueType::ArrayDouble: {
				void* const value = CreateArray<double>(pItem);
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			}
			case ValueType::ArrayString: {
				void* const value = CreateArray<plg::string>(pItem);
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			}
			case ValueType::Vector2: {
				void* const value = CreateValue<Vector2>(pItem);
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			}
			case ValueType::Vector3: {
				void* const value = CreateValue<Vector3>(pItem);
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			}
			case ValueType::Vector4: {
				void* const value = CreateValue<Vector4>(pItem);
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			}
			case ValueType::Matrix4x4: {
				void* const value = CreateValue<Matrix4x4>(pItem);
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			}
			default: {
				const std::string error(std::format("PushObjectAsParam unsupported type {:#x}", static_cast<uint8_t>(paramType.GetType())));
				PyErr_SetString(PyExc_RuntimeError, error.c_str());
				return false;
			}
			}

			return false;
		}

		bool PushObjectAsRefParam(PropertyRef paramType, PyObject* pItem, ArgsScope& a) {
			const auto PushRefParam = [&paramType, &a](void* value) {
				if (!value) {
					return false;
				}
				a.storage.emplace_back(value, paramType.GetType());
				a.params.AddArgument(value);
				return true;
			};

			switch (paramType.GetType()) {
			case ValueType::Bool:
				return PushRefParam(CreateValue<bool>(pItem));
			case ValueType::Char8:
				return PushRefParam(CreateValue<char>(pItem));
			case ValueType::Char16:
				return PushRefParam(CreateValue<char16_t>(pItem));
			case ValueType::Int8:
				return PushRefParam(CreateValue<int8_t>(pItem));
			case ValueType::Int16:
				return PushRefParam(CreateValue<int16_t>(pItem));
			case ValueType::Int32:
				return PushRefParam(CreateValue<int32_t>(pItem));
			case ValueType::Int64:
				return PushRefParam(CreateValue<int64_t>(pItem));
			case ValueType::UInt8:
				return PushRefParam(CreateValue<uint8_t>(pItem));
			case ValueType::UInt16:
				return PushRefParam(CreateValue<uint16_t>(pItem));
			case ValueType::UInt32:
				return PushRefParam(CreateValue<uint32_t>(pItem));
			case ValueType::UInt64:
				return PushRefParam(CreateValue<uint64_t>(pItem));
			case ValueType::Pointer:
				return PushRefParam(CreateValue<void*>(pItem));
			case ValueType::Float:
				return PushRefParam(CreateValue<float>(pItem));
			case ValueType::Double:
				return PushRefParam(CreateValue<double>(pItem));
			case ValueType::String:
				return PushRefParam(CreateValue<plg::string>(pItem));
			case ValueType::ArrayBool:
				return PushRefParam(CreateArray<bool>(pItem));
			case ValueType::ArrayChar8:
				return PushRefParam(CreateArray<char>(pItem));
			case ValueType::ArrayChar16:
				return PushRefParam(CreateArray<char16_t>(pItem));
			case ValueType::ArrayInt8:
				return PushRefParam(CreateArray<int8_t>(pItem));
			case ValueType::ArrayInt16:
				return PushRefParam(CreateArray<int16_t>(pItem));
			case ValueType::ArrayInt32:
				return PushRefParam(CreateArray<int32_t>(pItem));
			case ValueType::ArrayInt64:
				return PushRefParam(CreateArray<int64_t>(pItem));
			case ValueType::ArrayUInt8:
				return PushRefParam(CreateArray<uint8_t>(pItem));
			case ValueType::ArrayUInt16:
				return PushRefParam(CreateArray<uint16_t>(pItem));
			case ValueType::ArrayUInt32:
				return PushRefParam(CreateArray<uint32_t>(pItem));
			case ValueType::ArrayUInt64:
				return PushRefParam(CreateArray<uint64_t>(pItem));
			case ValueType::ArrayPointer:
				return PushRefParam(CreateArray<void*>(pItem));
			case ValueType::ArrayFloat:
				return PushRefParam(CreateArray<float>(pItem));
			case ValueType::ArrayDouble:
				return PushRefParam(CreateArray<double>(pItem));
			case ValueType::ArrayString:
				return PushRefParam(CreateArray<plg::string>(pItem));
			case ValueType::Vector2:
				return PushRefParam(CreateValue<Vector2>(pItem));
			case ValueType::Vector3:
				return PushRefParam(CreateValue<Vector3>(pItem));
			case ValueType::Vector4:
				return PushRefParam(CreateValue<Vector4>(pItem));
			case ValueType::Matrix4x4:
				return PushRefParam(CreateValue<Matrix4x4>(pItem));
			default: {
				const std::string error(std::format("PushObjectAsRefParam unsupported type {:#x}", static_cast<uint8_t>(paramType.GetType())));
				PyErr_SetString(PyExc_RuntimeError, error.c_str());
				return false;
			}
			}

			return false;
		}

		PyObject* StorageValueToObject(PropertyRef paramType, const ArgsScope& a, uint8_t index) {
			switch (paramType.GetType()) {
			case ValueType::Bool:
				return CreatePyObject(*reinterpret_cast<bool*>(std::get<0>(a.storage[index])));
			case ValueType::Char8:
				return CreatePyObject(*reinterpret_cast<char*>(std::get<0>(a.storage[index])));
			case ValueType::Char16:
				return CreatePyObject(*reinterpret_cast<char16_t*>(std::get<0>(a.storage[index])));
			case ValueType::Int8:
				return CreatePyObject(*reinterpret_cast<int8_t*>(std::get<0>(a.storage[index])));
			case ValueType::Int16:
				return CreatePyObject(*reinterpret_cast<int16_t*>(std::get<0>(a.storage[index])));
			case ValueType::Int32:
				return CreatePyObject(*reinterpret_cast<int32_t*>(std::get<0>(a.storage[index])));
			case ValueType::Int64:
				return CreatePyObject(*reinterpret_cast<int64_t*>(std::get<0>(a.storage[index])));
			case ValueType::UInt8:
				return CreatePyObject(*reinterpret_cast<uint8_t*>(std::get<0>(a.storage[index])));
			case ValueType::UInt16:
				return CreatePyObject(*reinterpret_cast<uint16_t*>(std::get<0>(a.storage[index])));
			case ValueType::UInt32:
				return CreatePyObject(*reinterpret_cast<uint32_t*>(std::get<0>(a.storage[index])));
			case ValueType::UInt64:
				return CreatePyObject(*reinterpret_cast<uint64_t*>(std::get<0>(a.storage[index])));
			case ValueType::Float:
				return CreatePyObject(*reinterpret_cast<float*>(std::get<0>(a.storage[index])));
			case ValueType::Double:
				return CreatePyObject(*reinterpret_cast<double*>(std::get<0>(a.storage[index])));
			case ValueType::String:
				return CreatePyObject(*reinterpret_cast<plg::string*>(std::get<0>(a.storage[index])));
			case ValueType::Pointer:
				return CreatePyObject(*reinterpret_cast<void**>(std::get<0>(a.storage[index])));
			case ValueType::ArrayBool:
				return CreatePyObjectList(*reinterpret_cast<plg::vector<bool>*>(std::get<0>(a.storage[index])));
			case ValueType::ArrayChar8:
				return CreatePyObjectList(*reinterpret_cast<plg::vector<char>*>(std::get<0>(a.storage[index])));
			case ValueType::ArrayChar16:
				return CreatePyObjectList(*reinterpret_cast<plg::vector<char16_t>*>(std::get<0>(a.storage[index])));
			case ValueType::ArrayInt8:
				return CreatePyObjectList(*reinterpret_cast<plg::vector<int8_t>*>(std::get<0>(a.storage[index])));
			case ValueType::ArrayInt16:
				return CreatePyObjectList(*reinterpret_cast<plg::vector<int16_t>*>(std::get<0>(a.storage[index])));
			case ValueType::ArrayInt32:
				return CreatePyObjectList(*reinterpret_cast<plg::vector<int32_t>*>(std::get<0>(a.storage[index])));
			case ValueType::ArrayInt64:
				return CreatePyObjectList(*reinterpret_cast<plg::vector<int64_t>*>(std::get<0>(a.storage[index])));
			case ValueType::ArrayUInt8:
				return CreatePyObjectList(*reinterpret_cast<plg::vector<uint8_t>*>(std::get<0>(a.storage[index])));
			case ValueType::ArrayUInt16:
				return CreatePyObjectList(*reinterpret_cast<plg::vector<uint16_t>*>(std::get<0>(a.storage[index])));
			case ValueType::ArrayUInt32:
				return CreatePyObjectList(*reinterpret_cast<plg::vector<uint32_t>*>(std::get<0>(a.storage[index])));
			case ValueType::ArrayUInt64:
				return CreatePyObjectList(*reinterpret_cast<plg::vector<uint64_t>*>(std::get<0>(a.storage[index])));
			case ValueType::ArrayPointer:
				return CreatePyObjectList(*reinterpret_cast<plg::vector<void*>*>(std::get<0>(a.storage[index])));
			case ValueType::ArrayFloat:
				return CreatePyObjectList(*reinterpret_cast<plg::vector<float>*>(std::get<0>(a.storage[index])));
			case ValueType::ArrayDouble:
				return CreatePyObjectList(*reinterpret_cast<plg::vector<double>*>(std::get<0>(a.storage[index])));
			case ValueType::ArrayString:
				return CreatePyObjectList(*reinterpret_cast<plg::vector<plg::string>*>(std::get<0>(a.storage[index])));
			case ValueType::Vector2:
				return CreatePyObject(*reinterpret_cast<Vector2*>(std::get<0>(a.storage[index])));
			case ValueType::Vector3:
				return CreatePyObject(*reinterpret_cast<Vector3*>(std::get<0>(a.storage[index])));
			case ValueType::Vector4:
				return CreatePyObject(*reinterpret_cast<Vector4*>(std::get<0>(a.storage[index])));
			case ValueType::Matrix4x4:
				return CreatePyObject(*reinterpret_cast<Matrix4x4*>(std::get<0>(a.storage[index])));
			default: {
				const std::string error(std::format("StorageValueToObject unsupported type {:#x}", static_cast<uint8_t>(paramType.GetType())));
				PyErr_SetString(PyExc_RuntimeError, error.c_str());
				return nullptr;
			}
			}
		}

		void ExternalCallNoArgs(MethodRef method, MemAddr data, const JitCallback::Parameters* p, uint8_t count, const JitCallback::Return* ret) {
			// PyObject* (MethodPyCall*)(PyObject* self, PyObject* args)
			ArgsScope a(1);
			JitCall::Return r;
			BeginExternalCall(method, a);
			PyObject* const retObj = MakeExternalCall(method, data.CCast<JitCall::CallingFunc>(), a, r);
			if (!retObj) {
				// MakeExternalCall set error
				ret->SetReturn(nullptr);
				return;
			}
			ret->SetReturn(retObj);
		}

		void ExternalCall(MethodRef method, MemAddr data, const JitCallback::Parameters* p, uint8_t count, const JitCallback::Return* ret) {
			// PyObject* (MethodPyCall*)(PyObject* self, PyObject* args)
			const auto args = p->GetArgument<PyObject*>(1);

			if (!PyTuple_Check(args)) {
				const std::string error(std::format("Function \"{}\" expects a tuple of arguments", method.GetFunctionName()));
				PyErr_SetString(PyExc_TypeError, error.c_str());
				ret->SetReturn(nullptr);
				return;
			}

			const auto paramTypes = method.GetParamTypes();
			const auto paramCount = static_cast<uint8_t>(paramTypes.size());
			const Py_ssize_t size = PyTuple_Size(args);
			if (size != static_cast<Py_ssize_t>(paramCount)) {
				const std::string error(std::format("Wrong number of parameters, {} when {} required.", size, paramCount));
				PyErr_SetString(PyExc_TypeError, error.c_str());
				ret->SetReturn(nullptr);
				return;
			}

			Py_ssize_t refParamsCount = 0;
			const Py_ssize_t paramsStartIndex = plugify::ValueUtils::IsHiddenParam(method.GetReturnType().GetType()) ? 1 : 0;

			ArgsScope a(1 + paramCount);
			JitCall::Return r;

			BeginExternalCall(method, a);

			for (Py_ssize_t i = 0; i < size; ++i) {
				const PropertyRef paramType = paramTypes[i];
				if (paramType.IsReference()) {
					++refParamsCount;
				}
				using PushParamFunc = bool (*)(PropertyRef, PyObject*, ArgsScope&);
				PushParamFunc const pushParamFunc = paramType.IsReference() ? &PushObjectAsRefParam : &PushObjectAsParam;
				const bool pushResult = pushParamFunc(paramType, PyTuple_GetItem(args, i), a);
				if (!pushResult) {
					// pushParamFunc set error
					ret->SetReturn(nullptr);
					return;
				}
			}

			PyObject* retObj = MakeExternalCall(method, data.CCast<JitCall::CallingFunc>(), a, r);
			if (!retObj) {
				// MakeExternalCall set error
				ret->SetReturn(nullptr);
				return;
			}

			if (refParamsCount) {
				PyObject* const retTuple = PyTuple_New(1 + refParamsCount);

				Py_ssize_t k = 0;

				PyTuple_SET_ITEM(retTuple, k++, retObj); // retObj ref taken by tuple

				for (Py_ssize_t i = 0, j = paramsStartIndex; i < paramCount; ++i) {
					const PropertyRef paramType = paramTypes[i];
					if (!paramType.IsReference()) {
						continue;
					}
					PyObject* const value = StorageValueToObject(paramType, a, j++);
					if (!value) {
						// StorageValueToObject set error
						Py_DECREF(retTuple);
						ret->SetReturn(nullptr);
						return;
					}
					PyTuple_SET_ITEM(retTuple, k++, value);
					if (k >= refParamsCount + 1) {
						break;
					}
				}

				retObj = retTuple;
			}

			ret->SetReturn(retObj);
		}

		template<typename T>
		std::optional<T> GetObjectAttrAsValue(PyObject* object, const char* attr_name) {
			PyObject* const attrObject = PyObject_GetAttrString(object, attr_name);
			if (!attrObject) {
				// PyObject_GetAttrString set error. e.g. AttributeError
				return std::nullopt;
			}
			const auto value = ValueFromObject<T>(attrObject);
			// ValueFromObject set error. e.g. TypeError
			Py_DECREF(attrObject);
			return value;
		}
	}

	Python3LanguageModule::Python3LanguageModule() = default;

	Python3LanguageModule::~Python3LanguageModule() = default;

	InitResult Python3LanguageModule::Initialize(std::weak_ptr<IPlugifyProvider> provider, ModuleRef module) {
		if (!(_provider = provider.lock())) {
			return ErrorData{ "Provider not exposed" };
		}

		_jitRuntime = std::make_shared<asmjit::JitRuntime>();

		std::error_code ec;
		const fs::path moduleBasePath = fs::absolute(module.GetBaseDir(), ec);
		if (ec) {
			return ErrorData{ "Failed to get module directory path" };
		}

		const fs::path libPath = moduleBasePath / "lib";
		if (!fs::exists(libPath, ec) || !fs::is_directory(libPath, ec)) {
			return ErrorData{ "lib directory not exists" };
		}

		const fs::path pythonBasePath = moduleBasePath / "python3.12";
		if (!fs::exists(pythonBasePath, ec) || !fs::is_directory(pythonBasePath, ec)) {
			return ErrorData{ "python3.12 directory not exists" };
		}

		const fs::path modulesZipPath = pythonBasePath / L"python312.zip";
		const fs::path pluginsPath = fs::weakly_canonical(moduleBasePath / ".." / ".." / "plugins", ec);
		if (ec) {
			return ErrorData{ "Failed to get plugins directory path" };
		}

		if (Py_IsInitialized()) {
			return ErrorData{ "Python already initialized" };
		}

		PyStatus status;

		PyConfig config{};
		PyConfig_InitIsolatedConfig(&config);

		for (;;) {
			status = PyConfig_SetString(&config, &config.home, pythonBasePath.wstring().c_str());
			if (PyStatus_Exception(status)) {
				break;
			}

			// Manually set search paths:
			// 1. python zip
			// 2. python dir
			// 3. lib dir in module
			// 4. plugins dir

			config.module_search_paths_set = 1;

			status = PyWideStringList_Append(&config.module_search_paths, modulesZipPath.wstring().c_str());
			if (PyStatus_Exception(status)) {
				break;
			}
			status = PyWideStringList_Append(&config.module_search_paths, pythonBasePath.wstring().c_str());
			if (PyStatus_Exception(status)) {
				break;
			}
			status = PyWideStringList_Append(&config.module_search_paths, libPath.wstring().c_str());
			if (PyStatus_Exception(status)) {
				break;
			}
			status = PyWideStringList_Append(&config.module_search_paths, pluginsPath.wstring().c_str());
			if (PyStatus_Exception(status)) {
				break;
			}

			status = Py_InitializeFromConfig(&config);

			break;
		}

		if (PyStatus_Exception(status)) {
			return ErrorData{ std::format("Failed to init python: {}", status.err_msg) };
		}

		PyObject* const plugifyPluginModuleName = PyUnicode_DecodeFSDefault("plugify.plugin");
		if (!plugifyPluginModuleName) {
			LogError();
			return ErrorData{ "Failed to allocate plugify.plugin module string" };
		}

		PyObject* const plugifyPluginModule = PyImport_Import(plugifyPluginModuleName);
		Py_DECREF(plugifyPluginModuleName);
		if (!plugifyPluginModule) {
			LogError();
			return ErrorData{ "Failed to import plugify.plugin python module" };
		}

		_PluginTypeObject = PyObject_GetAttrString(plugifyPluginModule, "Plugin");
		if (!_PluginTypeObject) {
			Py_DECREF(plugifyPluginModule);
			LogError();
			return ErrorData{ "Failed to find plugify.plugin.Plugin type" };
		}
		_PluginInfoTypeObject = PyObject_GetAttrString(plugifyPluginModule, "PluginInfo");
		if (!_PluginInfoTypeObject) {
			Py_DECREF(plugifyPluginModule);
			LogError();
			return ErrorData{ "Failed to find plugify.plugin.PluginInfo type" };
		}

		_Vector2TypeObject = PyObject_GetAttrString(plugifyPluginModule, "Vector2");
		if (!_Vector2TypeObject) {
			Py_DECREF(plugifyPluginModule);
			LogError();
			return ErrorData{ "Failed to find plugify.plugin.Vector2 type" };
		}
		_Vector3TypeObject = PyObject_GetAttrString(plugifyPluginModule, "Vector3");
		if (!_Vector3TypeObject) {
			Py_DECREF(plugifyPluginModule);
			LogError();
			return ErrorData{ "Failed to find plugify.plugin.Vector3 type" };
		}
		_Vector4TypeObject = PyObject_GetAttrString(plugifyPluginModule, "Vector4");
		if (!_Vector4TypeObject) {
			Py_DECREF(plugifyPluginModule);
			LogError();
			return ErrorData{ "Failed to find plugify.plugin.Vector4 type" };
		}
		_Matrix4x4TypeObject = PyObject_GetAttrString(plugifyPluginModule, "Matrix4x4");
		if (!_Matrix4x4TypeObject) {
			Py_DECREF(plugifyPluginModule);
			LogError();
			return ErrorData{ "Failed to find plugify.plugin.Matrix4x4 type" };
		}

		Py_DECREF(plugifyPluginModule);

		_ppsModule = PyImport_ImportModule("plugify.pps");
		if (!_ppsModule) {
			LogError();
			return ErrorData{ "Failed to import plugify.pps python module" };
		}

		PyObject* const tracebackModule = PyImport_ImportModule("traceback");
		if (!tracebackModule) {
			LogError();
			return ErrorData{ "Failed to import traceback python module" };
		}
		_formatException = PyObject_GetAttrString(tracebackModule, "format_exception");
		if (!_formatException) {
			Py_DECREF(tracebackModule);
			LogError();
			return ErrorData{ "Failed to import traceback.format_exception python module" };
		}

		Py_DECREF(tracebackModule);

		return InitResultData{};
	}

	void Python3LanguageModule::Shutdown() {
		if (Py_IsInitialized()) {			
			if (_formatException) {
				Py_DECREF(_formatException);
			}

			if (_ppsModule) {
				Py_DECREF(_ppsModule);
			}

			if (_Vector2TypeObject) {
				Py_DECREF(_Vector2TypeObject);
			}

			if (_Vector3TypeObject) {
				Py_DECREF(_Vector3TypeObject);
			}

			if (_Vector4TypeObject) {
				Py_DECREF(_Vector4TypeObject);
			}

			if (_Matrix4x4TypeObject) {
				Py_DECREF(_Matrix4x4TypeObject);
			}

			if (_PluginTypeObject) {
				Py_DECREF(_PluginTypeObject);
			}

			if (_PluginInfoTypeObject) {
				Py_DECREF(_PluginInfoTypeObject);
			}

			for (const auto& data : _internalFunctions) {
				Py_DECREF(data.pythonFunction);
			}

			for (const auto& [_1, _2, _3, object] : _externalFunctions) {
				Py_DECREF(object);
			}

			for (const auto& data : _pythonMethods) {
				Py_DECREF(data.pythonFunction);
			}

			for (const auto& [_, pluginData] : _pluginsMap) {
				Py_DECREF(pluginData._instance);
				Py_DECREF(pluginData._module);
			}

			Py_Finalize();
		}
		_formatException = nullptr;
		_ppsModule = nullptr;
		_Vector2TypeObject = nullptr;
		_Vector3TypeObject = nullptr;
		_Vector4TypeObject = nullptr;
		_Matrix4x4TypeObject = nullptr;
		_PluginTypeObject = nullptr;
		_PluginInfoTypeObject = nullptr;
		_internalMap.clear();
		_externalMap.clear();
		_internalFunctions.clear();
		_externalFunctions.clear();
		_moduleDefinitions.clear();
		_moduleMethods.clear();
		_moduleFunctions.clear();
		_pythonMethods.clear();
		_pluginsMap.clear();
		_jitRuntime.reset();
		_provider.reset();
	}

	void Python3LanguageModule::OnMethodExport(PluginRef plugin) {
		if (_ppsModule) {
			PyObject* moduleObject = CreateInternalModule(plugin);
			if (!moduleObject) {
				moduleObject = CreateExternalModule(plugin);
			}
			if (moduleObject) {
				PyObject_SetAttrString(_ppsModule, std::string(plugin.GetName()).c_str(), moduleObject);
				Py_DECREF(moduleObject);
				return;
			}
		}
		_provider->Log(std::format("[py3lm] Fail to export '{}' plugin methods", plugin.GetName()), Severity::Error);
	}

	LoadResult Python3LanguageModule::OnPluginLoad(PluginRef plugin) {
		const std::string_view entryPoint = plugin.GetDescriptor().GetEntryPoint();
		if (entryPoint.empty()) {
			return ErrorData{ "Incorrect entry point: empty" };
		}
		if (entryPoint.find_first_of("/\\") != std::string::npos) {
			return ErrorData{ "Incorrect entry point: contains '/' or '\\'" };
		}
		const std::string::size_type lastDotPos = entryPoint.find_last_of('.');
		if (lastDotPos == std::string::npos) {
			return ErrorData{ "Incorrect entry point: not have any dot '.' character" };
		}
		std::string_view className(entryPoint.begin() + (lastDotPos + 1), entryPoint.end());
		if (className.empty()) {
			return ErrorData{ "Incorrect entry point: empty class name part" };
		}
		std::string_view modulePathRel(entryPoint.begin(), entryPoint.begin() + lastDotPos);
		if (modulePathRel.empty()) {
			return ErrorData{ "Incorrect entry point: empty module path part" };
		}

		const fs::path baseFolder(plugin.GetBaseDir());
		auto modulePath = std::string(modulePathRel);
		ReplaceAll(modulePath, ".", { static_cast<char>(fs::path::preferred_separator) });
		fs::path filePathRelative = modulePath;
		filePathRelative.replace_extension(".py");
		const fs::path filePath = baseFolder / filePathRelative;
		std::error_code ec;
		if (!fs::exists(filePath, ec) || !fs::is_regular_file(filePath, ec)) {
			return ErrorData{ std::format("Module file '{}' not exist", filePath.string()) };
		}
		const fs::path pluginsFolder = baseFolder.parent_path();
		filePathRelative = fs::relative(filePath, pluginsFolder, ec);
		filePathRelative.replace_extension();
		std::string moduleName = filePathRelative.generic_string();
		ReplaceAll(moduleName, "/", ".");

		_provider->Log(std::format("[py3lm] Load plugin module '{}'", moduleName), Severity::Verbose);

		PyObject* const pluginModule = PyImport_ImportModule(moduleName.c_str());
		if (!pluginModule) {
			LogError();
			return ErrorData{ std::format("Failed to import {} module", moduleName) };
		}

		PyObject* const classNameString = PyUnicode_FromStringAndSize(className.data(), static_cast<Py_ssize_t>(className.size()));
		if (!classNameString) {
			Py_DECREF(pluginModule);
			return ErrorData{ "Allocate class name string failed" };
		}

		PyObject* const pluginClass = PyObject_GetAttr(pluginModule, classNameString);
		if (!pluginClass) {
			Py_DECREF(classNameString);
			Py_DECREF(pluginModule);
			LogError();
			return ErrorData{ "Failed to find plugin class" };
		}

		const int typeResult = PyObject_IsSubclass(pluginClass, _PluginTypeObject);
		if (typeResult != 1) {
			Py_DECREF(pluginClass);
			Py_DECREF(classNameString);
			Py_DECREF(pluginModule);
			LogError();
			return ErrorData{ std::format("Class '{}' not subclass of Plugin", className) };
		}

		PyObject* const pluginInstance = PyObject_CallNoArgs(pluginClass);
		Py_DECREF(pluginClass);
		if (!pluginInstance) {
			Py_DECREF(classNameString);
			Py_DECREF(pluginModule);
			LogError();
			return ErrorData{ "Failed to create plugin instance" };
		}

		PyObject* const args = PyTuple_New(Py_ssize_t{ 2 });
		if (!args) {
			Py_DECREF(pluginInstance);
			Py_DECREF(classNameString);
			Py_DECREF(pluginModule);
			return ErrorData{ "Failed to save instance: arguments tuple is null" };
		}

		PyTuple_SET_ITEM(args, Py_ssize_t{ 0 }, classNameString); // classNameString ref taken by list
		Py_INCREF(pluginInstance);
		PyTuple_SET_ITEM(args, Py_ssize_t{ 1 }, pluginInstance); // pluginInstance ref taken by list

		PyObject* const pluginInfo = PyObject_CallObject(_PluginInfoTypeObject, args);
		Py_DECREF(args);
		if (!pluginInfo) {
			Py_DECREF(pluginInstance);
			Py_DECREF(pluginModule);
			LogError();
			return ErrorData{ "Failed to save instance: plugin info not constructed" };
		}

		const int resultCode = PyObject_SetAttrString(pluginModule, "__plugin__", pluginInfo);
		Py_DECREF(pluginInfo);
		if (resultCode != 0) {
			Py_DECREF(pluginInstance);
			Py_DECREF(pluginModule);
			LogError();
			return ErrorData{ "Failed to save instance: assignment fail" };
		}

		if (_pluginsMap.contains(plugin.GetId())) {
			Py_DECREF(pluginInstance);
			Py_DECREF(pluginModule);
			return ErrorData{ "Plugin id duplicate" };
		}

		const auto exportedMethods = plugin.GetDescriptor().GetExportedMethods();
		bool exportResult = true;
		std::vector<std::string> exportErrors;
		std::vector<std::tuple<MethodRef, PythonMethodData>> methodsHolders;

		if (!exportedMethods.empty()) {
			for (const MethodRef method : exportedMethods) {
				MethodExportResult generateResult = GenerateMethodExport(method, _jitRuntime, pluginModule, pluginInstance);
				if (auto* data = std::get_if<MethodExportError>(&generateResult)) {
					exportResult = false;
					exportErrors.emplace_back(std::move(*data));
					continue;
				}
				methodsHolders.emplace_back(method, std::move(std::get<MethodExportData>(generateResult)));
			}
		}

		if (!exportResult) {
			Py_DECREF(pluginInstance);
			Py_DECREF(pluginModule);
			std::string errorString = "Methods export error(s): " + exportErrors[0];
			for (auto it = std::next(exportErrors.begin()); it != exportErrors.end(); ++it) {
				std::format_to(std::back_inserter(errorString), ", {}", *it);
			}
			return ErrorData{ std::move(errorString) };
		}

		const auto [_, result] = _pluginsMap.try_emplace(plugin.GetId(), pluginModule, pluginInstance);
		if (!result) {
			Py_DECREF(pluginInstance);
			Py_DECREF(pluginModule);
			return ErrorData{ std::format("Save plugin data to map unsuccessful") };
		}

		std::vector<MethodData> methods;
		methods.reserve(methodsHolders.size());
		_pythonMethods.reserve(methodsHolders.size());

		for (auto& [method, methodData] : methodsHolders) {
			const MemAddr methodAddr = methodData.jitCallback.GetFunction();
			methods.emplace_back(method, methodAddr);
			AddToFunctionsMap(methodAddr, methodData.pythonFunction);
			_pythonMethods.emplace_back(std::move(methodData));
		}

		return LoadResultData{ std::move(methods) };
	}

	void Python3LanguageModule::OnPluginStart(PluginRef plugin) {
		TryCallPluginMethodNoArgs(plugin, "plugin_start", "OnPluginStart");
	}

	void Python3LanguageModule::OnPluginEnd(PluginRef plugin) {
		TryCallPluginMethodNoArgs(plugin, "plugin_end", "OnPluginEnd");
	}

	bool Python3LanguageModule::IsDebugBuild() {
		return PY3LM_IS_DEBUG;
	}

	PyObject* Python3LanguageModule::FindExternal(void* funcAddr) const {
		const auto it = _externalMap.find(funcAddr);
		if (it != _externalMap.end()) {
			return std::get<PyObject*>(*it);
		}
		return nullptr;
	}

	void* Python3LanguageModule::FindInternal(PyObject* object) const {
		const auto it = _internalMap.find(object);
		if (it != _internalMap.end()) {
			return std::get<void*>(*it);
		}
		return nullptr;
	}

	void Python3LanguageModule::AddToFunctionsMap(void* funcAddr, PyObject* object) {
		_externalMap.emplace(funcAddr, object);
		_internalMap.emplace(object, funcAddr);
	}

	PyObject* Python3LanguageModule::GetOrCreateFunctionObject(MethodRef method, void* funcAddr) {
		if (PyObject* const object = FindExternal(funcAddr)) {
			Py_INCREF(object);
			return object;
		}
		JitCall call(_jitRuntime);

		const MemAddr callAddr = call.GetJitFunc(method, funcAddr);
		if (!callAddr) {
			const std::string error(std::format("Lang module JIT failed to generate c++ call wrapper '{}'", call.GetError()));
			PyErr_SetString(PyExc_RuntimeError, error.c_str());
			return nullptr;
		}

		JitCallback callback(_jitRuntime);

		asmjit::FuncSignature sig(asmjit::CallConvId::kCDecl);
		sig.addArg(asmjit::TypeId::kUIntPtr);
		sig.addArg(asmjit::TypeId::kUIntPtr);
		sig.setRet(asmjit::TypeId::kUIntPtr);

		const bool noArgs = method.GetParamTypes().empty();

		const MemAddr methodAddr = callback.GetJitFunc(sig, method, noArgs ? &ExternalCallNoArgs : &ExternalCall, callAddr, false);
		if (!methodAddr) {
			const std::string error(std::format("Lang module JIT failed to generate c++ PyCFunction wrapper '{}'", callback.GetError()));
			PyErr_SetString(PyExc_RuntimeError, error.c_str());
			return nullptr;
		}

		auto defPtr = std::make_unique<PyMethodDef>();
		PyMethodDef& def = *(defPtr.get());
		def.ml_name = "PlugifyExternal";
		def.ml_meth = methodAddr.RCast<PyCFunction>();
		def.ml_flags = noArgs ? METH_NOARGS : METH_VARARGS;
		def.ml_doc = nullptr;

		PyObject* const object = PyCFunction_New(defPtr.get(), nullptr);
		if (!object) {
			PyErr_SetString(PyExc_RuntimeError, "Fail to create function object from function pointer");
			return nullptr;
		}

		Py_INCREF(object);
		_externalFunctions.emplace_back(std::move(callback), std::move(call), std::move(defPtr), object);
		AddToFunctionsMap(funcAddr, object);

		return object;
	}

	std::optional<void*> Python3LanguageModule::GetOrCreateFunctionValue(MethodRef method, PyObject* object) {
		if (object == Py_None) {
			return { nullptr };
		}

		if (!PyFunction_Check(object) && !PyCallable_Check(object)) {
			PyErr_SetString(PyExc_TypeError, "Not function");
			return std::nullopt;
		}

		if (void* const funcAddr = FindInternal(object)) {
			return { funcAddr };
		}

		auto [result, callback] = CreateInternalCall(_jitRuntime, method, object);
		if (!result) {
			const std::string error(std::format("Lang module JIT failed to generate C++ wrapper from callback object '{}'", callback.GetError()));
			PyErr_SetString(PyExc_RuntimeError, error.c_str());
			return std::nullopt;
		}

		void* const funcAddr = callback.GetFunction();

		Py_INCREF(object);
		_internalFunctions.emplace_back(std::move(callback), object);
		AddToFunctionsMap(funcAddr, object);

		return { funcAddr };
	}

	PyObject* Python3LanguageModule::CreateVector2Object(const Vector2& vector) {
		PyObject* const args = PyTuple_New(Py_ssize_t{ 2 });
		if (!args) {
			PyErr_SetString(PyExc_RuntimeError, "Fail to create arguments tuple");
			return nullptr;
		}
		// CreatePyObject set error
		PyObject* const xObject = CreatePyObject(vector.x);
		if (!xObject) {
			Py_DECREF(args);
			return nullptr;
		}
		PyTuple_SET_ITEM(args, Py_ssize_t{ 0 }, xObject); // xObject ref taken by tuple
		PyObject* const yObject = CreatePyObject(vector.y);
		if (!yObject) {
			Py_DECREF(args);
			return nullptr;
		}
		PyTuple_SET_ITEM(args, Py_ssize_t{ 1 }, yObject); // yObject ref taken by tuple
		PyObject* const vectorObject = PyObject_CallObject(_Vector2TypeObject, args);
		Py_DECREF(args);
		return vectorObject;
	}

	std::optional<Vector2> Python3LanguageModule::Vector2ValueFromObject(PyObject* object) {
		const int typeResult = PyObject_IsInstance(object, _Vector2TypeObject);
		if (typeResult == -1) {
			// Python exception was set by PyObject_IsInstance
			return std::nullopt;
		}
		if (typeResult == 0) {
			PyErr_SetString(PyExc_TypeError, "Not Vector2");
			return std::nullopt;
		}
		auto xValue = GetObjectAttrAsValue<float>(object, "x");
		if (!xValue) {
			// GetObjectAttrAsValue set error. e.g. AttributeError, TypeError, ValueError
			return std::nullopt;
		}
		auto yValue = GetObjectAttrAsValue<float>(object, "y");
		if (!yValue) {
			// GetObjectAttrAsValue set error. e.g. AttributeError, TypeError, ValueError
			return std::nullopt;
		}
		return Vector2{ *xValue, *yValue };
	}

	PyObject* Python3LanguageModule::CreateVector3Object(const Vector3& vector) {
		PyObject* const args = PyTuple_New(Py_ssize_t{ 3 });
		if (!args) {
			PyErr_SetString(PyExc_RuntimeError, "Fail to create arguments tuple");
			return nullptr;
		}
		// CreatePyObject set error
		PyObject* const xObject = CreatePyObject(vector.x);
		if (!xObject) {
			Py_DECREF(args);
			return nullptr;
		}
		PyTuple_SET_ITEM(args, Py_ssize_t{ 0 }, xObject); // xObject ref taken by tuple
		PyObject* const yObject = CreatePyObject(vector.y);
		if (!yObject) {
			Py_DECREF(args);
			return nullptr;
		}
		PyTuple_SET_ITEM(args, Py_ssize_t{ 1 }, yObject); // yObject ref taken by tuple
		PyObject* const zObject = CreatePyObject(vector.z);
		if (!zObject) {
			Py_DECREF(args);
			return nullptr;
		}
		PyTuple_SET_ITEM(args, Py_ssize_t{ 2 }, zObject); // zObject ref taken by tuple
		PyObject* const vectorObject = PyObject_CallObject(_Vector3TypeObject, args);
		Py_DECREF(args);
		return vectorObject;
	}

	std::optional<Vector3> Python3LanguageModule::Vector3ValueFromObject(PyObject* object) {
		const int typeResult = PyObject_IsInstance(object, _Vector3TypeObject);
		if (typeResult == -1) {
			// Python exception was set by PyObject_IsInstance
			return std::nullopt;
		}
		if (typeResult == 0) {
			PyErr_SetString(PyExc_TypeError, "Not Vector3");
			return std::nullopt;
		}
		auto xValue = GetObjectAttrAsValue<float>(object, "x");
		if (!xValue) {
			// GetObjectAttrAsValue set error. e.g. AttributeError, TypeError, ValueError
			return std::nullopt;
		}
		auto yValue = GetObjectAttrAsValue<float>(object, "y");
		if (!yValue) {
			// GetObjectAttrAsValue set error. e.g. AttributeError, TypeError, ValueError
			return std::nullopt;
		}
		auto zValue = GetObjectAttrAsValue<float>(object, "z");
		if (!zValue) {
			// GetObjectAttrAsValue set error. e.g. AttributeError, TypeError, ValueError
			return std::nullopt;
		}
		return Vector3{ *xValue, *yValue, *zValue };
	}

	PyObject* Python3LanguageModule::CreateVector4Object(const Vector4& vector) {
		PyObject* const args = PyTuple_New(Py_ssize_t{ 4 });
		if (!args) {
			PyErr_SetString(PyExc_RuntimeError, "Fail to create arguments tuple");
			return nullptr;
		}
		// CreatePyObject set error
		PyObject* const xObject = CreatePyObject(vector.x);
		if (!xObject) {
			Py_DECREF(args);
			return nullptr;
		}
		PyTuple_SET_ITEM(args, Py_ssize_t{ 0 }, xObject); // xObject ref taken by tuple
		PyObject* const yObject = CreatePyObject(vector.y);
		if (!yObject) {
			Py_DECREF(args);
			return nullptr;
		}
		PyTuple_SET_ITEM(args, Py_ssize_t{ 1 }, yObject); // yObject ref taken by tuple
		PyObject* const zObject = CreatePyObject(vector.z);
		if (!zObject) {
			Py_DECREF(args);
			return nullptr;
		}
		PyTuple_SET_ITEM(args, Py_ssize_t{ 2 }, zObject); // zObject ref taken by tuple
		PyObject* const wObject = CreatePyObject(vector.w);
		if (!wObject) {
			Py_DECREF(args);
			return nullptr;
		}
		PyTuple_SET_ITEM(args, Py_ssize_t{ 3 }, wObject); // wObject ref taken by tuple
		PyObject* const vectorObject = PyObject_CallObject(_Vector4TypeObject, args);
		Py_DECREF(args);
		return vectorObject;
	}

	std::optional<Vector4> Python3LanguageModule::Vector4ValueFromObject(PyObject* object) {
		const int typeResult = PyObject_IsInstance(object, _Vector4TypeObject);
		if (typeResult == -1) {
			// Python exception was set by PyObject_IsInstance
			return std::nullopt;
		}
		if (typeResult == 0) {
			PyErr_SetString(PyExc_TypeError, "Not Vector4");
			return std::nullopt;
		}
		auto xValue = GetObjectAttrAsValue<float>(object, "x");
		if (!xValue) {
			// GetObjectAttrAsValue set error. e.g. AttributeError, TypeError, ValueError
			return std::nullopt;
		}
		auto yValue = GetObjectAttrAsValue<float>(object, "y");
		if (!yValue) {
			// GetObjectAttrAsValue set error. e.g. AttributeError, TypeError, ValueError
			return std::nullopt;
		}
		auto zValue = GetObjectAttrAsValue<float>(object, "z");
		if (!zValue) {
			// GetObjectAttrAsValue set error. e.g. AttributeError, TypeError, ValueError
			return std::nullopt;
		}
		auto wValue = GetObjectAttrAsValue<float>(object, "w");
		if (!wValue) {
			// GetObjectAttrAsValue set error. e.g. AttributeError, TypeError, ValueError
			return std::nullopt;
		}
		return Vector4{ *xValue, *yValue, *zValue, *wValue };
	}

	PyObject* Python3LanguageModule::CreateMatrix4x4Object(const Matrix4x4& matrix) {
		PyObject* const elementsObject = PyList_New(Py_ssize_t{ 16 });
		if (!elementsObject) {
			PyErr_SetString(PyExc_RuntimeError, "Fail to create Matrix4x4 elements list");
			return nullptr;
		}
		// CreatePyObject set error
		PyObject* const m00Object = CreatePyObject(matrix.m00);
		if (!m00Object) {
			Py_DECREF(elementsObject);
			return nullptr;
		}
		PyList_SET_ITEM(elementsObject, Py_ssize_t{ 0 }, m00Object); // m00Object ref taken by list
		PyObject* const m01Object = CreatePyObject(matrix.m01);
		if (!m01Object) {
			Py_DECREF(elementsObject);
			return nullptr;
		}
		PyList_SET_ITEM(elementsObject, Py_ssize_t{ 1 }, m01Object); // m01Object ref taken by list
		PyObject* const m02Object = CreatePyObject(matrix.m02);
		if (!m02Object) {
			Py_DECREF(elementsObject);
			return nullptr;
		}
		PyList_SET_ITEM(elementsObject, Py_ssize_t{ 2 }, m02Object); // m02Object ref taken by list
		PyObject* const m03Object = CreatePyObject(matrix.m03);
		if (!m03Object) {
			Py_DECREF(elementsObject);
			return nullptr;
		}
		PyList_SET_ITEM(elementsObject, Py_ssize_t{ 3 }, m03Object); // m03Object ref taken by list
		PyObject* const m10Object = CreatePyObject(matrix.m10);
		if (!m10Object) {
			Py_DECREF(elementsObject);
			return nullptr;
		}
		PyList_SET_ITEM(elementsObject, Py_ssize_t{ 4 }, m10Object); // m10Object ref taken by list
		PyObject* const m11Object = CreatePyObject(matrix.m11);
		if (!m11Object) {
			Py_DECREF(elementsObject);
			return nullptr;
		}
		PyList_SET_ITEM(elementsObject, Py_ssize_t{ 5 }, m11Object); // m11Object ref taken by list
		PyObject* const m12Object = CreatePyObject(matrix.m12);
		if (!m12Object) {
			Py_DECREF(elementsObject);
			return nullptr;
		}
		PyList_SET_ITEM(elementsObject, Py_ssize_t{ 6 }, m12Object); // m12Object ref taken by list
		PyObject* const m13Object = CreatePyObject(matrix.m13);
		if (!m13Object) {
			Py_DECREF(elementsObject);
			return nullptr;
		}
		PyList_SET_ITEM(elementsObject, Py_ssize_t{ 7 }, m13Object); // m13Object ref taken by list
		PyObject* const m20Object = CreatePyObject(matrix.m20);
		if (!m20Object) {
			Py_DECREF(elementsObject);
			return nullptr;
		}
		PyList_SET_ITEM(elementsObject, Py_ssize_t{ 8 }, m20Object); // m20Object ref taken by list
		PyObject* const m21Object = CreatePyObject(matrix.m21);
		if (!m21Object) {
			Py_DECREF(elementsObject);
			return nullptr;
		}
		PyList_SET_ITEM(elementsObject, Py_ssize_t{ 9 }, m21Object); // m21Object ref taken by list
		PyObject* const m22Object = CreatePyObject(matrix.m22);
		if (!m22Object) {
			Py_DECREF(elementsObject);
			return nullptr;
		}
		PyList_SET_ITEM(elementsObject, Py_ssize_t{ 10 }, m22Object); // m22Object ref taken by list
		PyObject* const m23Object = CreatePyObject(matrix.m23);
		if (!m23Object) {
			Py_DECREF(elementsObject);
			return nullptr;
		}
		PyList_SET_ITEM(elementsObject, Py_ssize_t{ 11 }, m23Object); // m23Object ref taken by list
		PyObject* const m30Object = CreatePyObject(matrix.m30);
		if (!m30Object) {
			Py_DECREF(elementsObject);
			return nullptr;
		}
		PyList_SET_ITEM(elementsObject, Py_ssize_t{ 12 }, m30Object); // m30Object ref taken by list
		PyObject* const m31Object = CreatePyObject(matrix.m31);
		if (!m31Object) {
			Py_DECREF(elementsObject);
			return nullptr;
		}
		PyList_SET_ITEM(elementsObject, Py_ssize_t{ 13 }, m31Object); // m31Object ref taken by list
		PyObject* const m32Object = CreatePyObject(matrix.m32);
		if (!m32Object) {
			Py_DECREF(elementsObject);
			return nullptr;
		}
		PyList_SET_ITEM(elementsObject, Py_ssize_t{ 14 }, m32Object); // m32Object ref taken by list
		PyObject* const m33Object = CreatePyObject(matrix.m33);
		if (!m33Object) {
			Py_DECREF(elementsObject);
			return nullptr;
		}
		PyList_SET_ITEM(elementsObject, Py_ssize_t{ 15 }, m33Object); // m33Object ref taken by list
		PyObject* const args = PyTuple_New(Py_ssize_t{ 1 });
		if (!args) {
			Py_DECREF(elementsObject);
			PyErr_SetString(PyExc_RuntimeError, "Fail to create arguments tuple");
			return nullptr;
		}
		PyTuple_SET_ITEM(args, Py_ssize_t{ 0 }, elementsObject); // elementsObject ref taken by tuple
		PyObject* const vectorObject = PyObject_CallObject(_Matrix4x4TypeObject, args);
		Py_DECREF(args);
		return vectorObject;
	}

	std::optional<Matrix4x4> Python3LanguageModule::Matrix4x4ValueFromObject(PyObject* object) {
		const int typeResult = PyObject_IsInstance(object, _Matrix4x4TypeObject);
		if (typeResult == -1) {
			// Python exception was set by PyObject_IsInstance
			return std::nullopt;
		}
		if (typeResult == 0) {
			PyErr_SetString(PyExc_TypeError, "Not Matrix4x4");
			return std::nullopt;
		}
		PyObject* const elementsListObject = PyObject_GetAttrString(object, "elements");
		if (!elementsListObject) {
			// PyObject_GetAttrString set error. e.g. AttributeError
			return std::nullopt;
		}
		if (!PyList_CheckExact(elementsListObject)) {
			Py_DECREF(elementsListObject);
			PyErr_SetString(PyExc_ValueError, "Elements must be a 4x4 list");
			return std::nullopt;
		}
		if (PyList_Size(elementsListObject) != Py_ssize_t{ 4 }) {
			Py_DECREF(elementsListObject);
			PyErr_SetString(PyExc_ValueError, "Elements must be a 4x4 list");
			return std::nullopt;
		}
		Matrix4x4 matrix{};
		for (Py_ssize_t i = 0; i < Py_ssize_t{ 4 }; ++i) {
			PyObject* const elementsRowListObject = PyList_GetItem(elementsListObject, i);
			if (!elementsRowListObject) [[unlikely]] {
				Py_DECREF(elementsListObject);
				// PyList_GetItem set error. e.g. IndexError
				return std::nullopt;
				}
				if (!PyList_CheckExact(elementsRowListObject)) {
					Py_DECREF(elementsListObject);
					PyErr_SetString(PyExc_ValueError, "Elements must be a 4x4 list");
					return std::nullopt;
				}
				if (PyList_Size(elementsRowListObject) != Py_ssize_t{ 4 }) {
					Py_DECREF(elementsListObject);
					PyErr_SetString(PyExc_ValueError, "Elements must be a 4x4 list");
					return std::nullopt;
				}
				for (Py_ssize_t j = 0; j < Py_ssize_t{ 4 }; ++j) {
					PyObject* const mObject = PyList_GetItem(elementsRowListObject, j);
					if (!mObject) [[unlikely]] {
						Py_DECREF(elementsListObject);
						/// PyList_GetItem set error. e.g. IndexError
						return std::nullopt;
						}
					const auto mValue = ValueFromObject<float>(mObject);
					if (!mValue) {
						Py_DECREF(elementsListObject);
						// ValueFromObject set error. e.g. TypeError
						return std::nullopt;
					}
					matrix.data[static_cast<size_t>(i * Py_ssize_t{ 4 } + j)] = *mValue;
				}
		}
		return { std::move(matrix) };
	}

	PyObject* Python3LanguageModule::FindPythonMethod(MemAddr addr) const {
		for (const auto& data : _pythonMethods) {
			if (data.jitCallback.GetFunction() == addr) {
				return data.pythonFunction;
			}
		}
		return nullptr;
	}

	PyObject* Python3LanguageModule::CreateInternalModule(PluginRef plugin) {
		if (!_pluginsMap.contains(plugin.GetId())) {
			return nullptr;
		}

		PyObject* moduleObject = PyModule_New(plugin.GetName().data());

		for (const auto& [method, addr] : plugin.GetMethods()) {
			PyObject* const methodObject = FindPythonMethod(addr);
			if (!methodObject) {
				_provider->Log(std::format("[py3lm] Not found '{}' method while CreateInternalModule for '{}' plugin", method.GetName(), plugin.GetName()), Severity::Fatal);
				std::terminate();
			}
			PyObject_SetAttrString(moduleObject, method.GetName().data(), methodObject);
		}

		return moduleObject;
	}

	PyObject* Python3LanguageModule::CreateExternalModule(PluginRef plugin) {
		auto& moduleMethods = _moduleMethods.emplace_back();

		for (const auto& [method, addr] : plugin.GetMethods()) {
			JitCall call(_jitRuntime);

			const MemAddr callAddr = call.GetJitFunc(method, addr);
			if (!callAddr) {
				const std::string error(std::format("Lang module JIT failed to generate c++ call wrapper '{}'", call.GetError()));
				PyErr_SetString(PyExc_RuntimeError, error.c_str());
				return nullptr;
			}

			JitCallback callback(_jitRuntime);

			asmjit::FuncSignature sig(asmjit::CallConvId::kCDecl);
			sig.addArg(asmjit::TypeId::kUIntPtr);
			sig.addArg(asmjit::TypeId::kUIntPtr);
			sig.setRet(asmjit::TypeId::kUIntPtr);

			const bool noArgs = method.GetParamTypes().empty();

			// Generate function --> PyObject* (MethodPyCall*)(PyObject* self, PyObject* args)
			const MemAddr methodAddr = callback.GetJitFunc(sig, method, noArgs ? &ExternalCallNoArgs : &ExternalCall, callAddr, false);
			if (!methodAddr)
				break;

			PyMethodDef& def = moduleMethods.emplace_back();
			def.ml_name = method.GetName().data();
			def.ml_meth = methodAddr.RCast<PyCFunction>();
			def.ml_flags = noArgs ? METH_NOARGS : METH_VARARGS;
			def.ml_doc = nullptr;

			_moduleFunctions.emplace_back(std::move(callback), std::move(call));
		}

		{
			PyMethodDef& def = moduleMethods.emplace_back();
			def.ml_name = nullptr;
			def.ml_meth = nullptr;
			def.ml_flags = 0;
			def.ml_doc = nullptr;
		}

		PyModuleDef& moduleDef = *(_moduleDefinitions.emplace_back(std::make_unique<PyModuleDef>()));
		moduleDef.m_base = PyModuleDef_HEAD_INIT;
		moduleDef.m_name = plugin.GetName().data();
		moduleDef.m_doc = nullptr;
		moduleDef.m_size = -1;
		moduleDef.m_methods = moduleMethods.data();
		moduleDef.m_slots = nullptr;
		moduleDef.m_traverse = nullptr;
		moduleDef.m_clear = nullptr;
		moduleDef.m_free = nullptr;

		return PyModule_Create(&moduleDef);
	}

	void Python3LanguageModule::TryCallPluginMethodNoArgs(PluginRef plugin, const std::string& name, const std::string& context) {
		const auto it = _pluginsMap.find(plugin.GetId());
		if (it == _pluginsMap.end()) {
			_provider->Log(std::format("[py3lm] {}: plugin '{}' not found in map", context, plugin.GetName()), Severity::Error);
			return;
		}

		const auto& pluginData = std::get<PluginData>(*it);
		if (!pluginData._instance) {
			_provider->Log(std::format("[py3lm] {}: null plugin instance", context), Severity::Error);
			return;
		}

		PyObject* const nameString = PyUnicode_DecodeFSDefault(name.c_str());
		if (!nameString) {
			LogError();
			_provider->Log(std::format("[py3lm] {}: failed to allocate name string", context), Severity::Error);
			return;
		}

		if (PyObject_HasAttr(pluginData._instance, nameString)) {
			PyObject* const returnObject = PyObject_CallMethodNoArgs(pluginData._instance, nameString);
			if (!returnObject) {
				LogError();
				_provider->Log(std::format("[py3lm] {}: call '{}' failed", context, name), Severity::Error);
			}
		}

		Py_DECREF(nameString);

		return;
	}

	void Python3LanguageModule::LogError() const {
		PyObject* ptype = nullptr;
		PyObject* pvalue = nullptr;
		PyObject* ptraceback = nullptr;
		PyErr_Fetch(&ptype, &pvalue, &ptraceback);
		std::string result;
		if (!pvalue) {
			Py_INCREF(Py_None);
			pvalue = Py_None;
		}
		if (!ptraceback) {
			Py_INCREF(Py_None);
			ptraceback = Py_None;
		}
		PyErr_NormalizeException(&ptype, &pvalue, &ptraceback);
		PyObject* strList = PyObject_CallFunctionObjArgs(_formatException, ptype, pvalue, ptraceback, nullptr);
		Py_DECREF(ptype);
		Py_DECREF(pvalue);
		Py_DECREF(ptraceback);
		if (!strList) {
			_provider->Log("Couldn't get exact error message", Severity::Error);
			return;
		}

		if (PySequence_Check(strList)) {
			PyObject* strList_fast = PySequence_Fast(strList, "Shouldn't happen (1)");
			PyObject** items = PySequence_Fast_ITEMS(strList_fast);
			Py_ssize_t L = PySequence_Fast_GET_SIZE(strList_fast);
			for (Py_ssize_t i = 0; i < L; ++i) {
				PyObject* utf8 = PyUnicode_AsUTF8String(items[i]);
				result += PyBytes_AsString(utf8);
				Py_DECREF(utf8);
			}
			Py_DECREF(strList_fast);
		} else {
			result = "Can't get exact error message";
		}

		Py_DECREF(strList);

		_provider->Log(result, Severity::Error);
	}

	void Python3LanguageModule::LogFatal(std::string_view msg) const {
		_provider->Log(msg, Severity::Fatal);
	}

	Python3LanguageModule g_py3lm;

	extern "C"
	PY3LM_EXPORT ILanguageModule* GetLanguageModule() {
		return &g_py3lm;
	}
}
