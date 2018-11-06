//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include "value_for_specialization.h"

namespace cfg
{
	// bool 특수화
	template<typename Ttype>
	class Value<Ttype, typename std::enable_if<std::is_same<Ttype, bool>::value>::type>
		: public ValueContainer<Ttype>
	{
	public:
		Value() : ValueContainer<Ttype>(Tconfig_type)
		{
			_value = std::make_shared<Ttype>();
		}

		Value(const Ttype &value) : Value() // NOLINT
		{
			_value = std::make_shared<Ttype>(value);
		}

		void Reset() override
		{
			_value = std::make_shared<Ttype>();
		}

		bool ParseFromValue(const ValueBase *value, int indent) override
		{
			auto from_value = dynamic_cast<const Value<Ttype> *>(value);

			if(from_value == nullptr)
			{
				return false;
			}

			*_value = *(from_value->_value);
			_is_parsed = true;

			return true;
		}

		bool ParseFromAttribute(const pugi::xml_attribute &attribute, bool processing_include_file, int indent) override
		{
			*_value = ov::Converter::ToBool(attribute.value());
			_is_parsed = true;

			return true;
		}

		bool ParseFromNode(const pugi::xml_node &node, bool processing_include_file, int indent) override
		{
			*_value = ov::Converter::ToBool(node.child_value());
			_is_parsed = true;

			return true;
		}

		ov::String ToStringInternal(int indent, bool append_new_line) const override
		{
			if(append_new_line)
			{
				return *_value ? "true\n" : "false\n";
			}

			return *_value ? "true" : "false";
		}

	protected:
		static constexpr ValueType Tconfig_type = ValueType::Boolean;
		using ValueContainer<Ttype>::_value;
		using ValueContainer<Ttype>::_is_parsed;
	};
}