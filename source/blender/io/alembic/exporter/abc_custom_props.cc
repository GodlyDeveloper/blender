/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2020 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup Alembic
 */

#include "abc_custom_props.h"

#include <functional>
#include <iostream>
#include <memory>
#include <string>

#include <Alembic/Abc/OTypedArrayProperty.h>
#include <Alembic/Abc/OTypedScalarProperty.h>

#include "BKE_idprop.h"
#include "DNA_ID.h"

using Alembic::Abc::ArraySample;
using Alembic::Abc::OArrayProperty;
using Alembic::Abc::OBoolArrayProperty;
using Alembic::Abc::ODoubleArrayProperty;
using Alembic::Abc::OFloatArrayProperty;
using Alembic::Abc::OInt32ArrayProperty;
using Alembic::Abc::OStringArrayProperty;

namespace blender::io::alembic {

CustomPropertiesExporter::CustomPropertiesExporter(
    Alembic::Abc::OCompoundProperty abc_compound_prop, uint32_t timesample_index)
    : abc_compound_prop_(abc_compound_prop), timesample_index_(timesample_index)
{
}

CustomPropertiesExporter::~CustomPropertiesExporter()
{
}

void CustomPropertiesExporter::write_all(IDProperty *group)
{
  if (group == nullptr) {
    return;
  }
  BLI_assert(group->type = IDP_GROUP);

  /* Loop over the properties, just like IDP_foreach_property() does, but without the recursion. */
  LISTBASE_FOREACH (IDProperty *, id_property, &group->data.group) {
    if (STREQ(id_property->name, "_RNA_UI")) {
      continue;
    }
    write(id_property);
  }
}

void CustomPropertiesExporter::write(IDProperty *id_property)
{
  BLI_assert(id_property->name[0] != '\0');

  switch (id_property->type) {
    case IDP_STRING: {
      /* The Alembic library doesn't accept NULL-terminated character arrays. */
      const std::string prop_value(IDP_String(id_property), id_property->len - 1);
      set_scalar_property<OStringArrayProperty, std::string>(id_property->name, prop_value);
      break;
    }
    case IDP_INT:
      static_assert(sizeof(int) == sizeof(int32_t), "Expecting 'int' to be 32-bit");
      set_scalar_property<OInt32ArrayProperty, int32_t>(id_property->name, IDP_Int(id_property));
      break;
    case IDP_FLOAT:
      set_scalar_property<OFloatArrayProperty, float>(id_property->name, IDP_Float(id_property));
      break;
    case IDP_DOUBLE:
      set_scalar_property<ODoubleArrayProperty, double>(id_property->name,
                                                        IDP_Double(id_property));
      break;
    case IDP_ARRAY:
      write_array(id_property);
      break;
    case IDP_IDPARRAY:
      write_idparray(id_property);
      break;
  }
}

void CustomPropertiesExporter::write_array(IDProperty *id_property)
{
  BLI_assert(id_property->type == IDP_ARRAY);

  switch (id_property->subtype) {
    case IDP_INT: {
      const int *array = (int *)IDP_Array(id_property);
      static_assert(sizeof(int) == sizeof(int32_t), "Expecting 'int' to be 32-bit");
      set_array_property<OInt32ArrayProperty, int32_t>(id_property->name, array, id_property->len);
      break;
    }
    case IDP_FLOAT: {
      const float *array = (float *)IDP_Array(id_property);
      set_array_property<OFloatArrayProperty, float>(id_property->name, array, id_property->len);
      break;
    }
    case IDP_DOUBLE: {
      const double *array = (double *)IDP_Array(id_property);
      set_array_property<ODoubleArrayProperty, double>(id_property->name, array, id_property->len);
      break;
    }
  }
}

void CustomPropertiesExporter::write_idparray(IDProperty *idp_array)
{
  BLI_assert(idp_array->type == IDP_IDPARRAY);

  if (idp_array->len == 0) {
    /* Don't bother writing dataless arrays. */
    return;
  }

  IDProperty *idp_elements = (IDProperty *)IDP_Array(idp_array);

#ifndef NDEBUG
  /* Sanity check that all elements of the array have the same type.
   * Blender should already enforce this, hence it's only used in debug mode. */
  for (int i = 1; i < idp_array->len; i++) {
    if (idp_elements[i].type == idp_elements[0].type) {
      continue;
    }
    std::cerr << "Custom property " << idp_array->name << " has elements of varying type";
    BLI_assert(!"Mixed type IDP_ARRAY custom property found");
  }
#endif

  switch (idp_elements[0].type) {
    case IDP_STRING:
      write_idparray_of_strings(idp_array);
      break;
    case IDP_ARRAY:
      write_idparray_of_numbers(idp_array);
      break;
  }
}

void CustomPropertiesExporter::write_idparray_of_strings(IDProperty *idp_array)
{
  BLI_assert(idp_array->type == IDP_IDPARRAY);
  BLI_assert(idp_array->len > 0);

  /* Convert to an array of std::strings, because Alembic doesn't like zero-delimited strings. */
  IDProperty *idp_elements = (IDProperty *)IDP_Array(idp_array);
  std::vector<std::string> strings(idp_array->len);
  for (int i = 0; i < idp_array->len; i++) {
    BLI_assert(idp_elements[i].type == IDP_STRING);
    strings[i] = IDP_String(&idp_elements[i]);
  }

  /* Alembic needs a pointer to the first value of the array. */
  const std::string *array_of_strings = &strings[0];
  set_array_property<OStringArrayProperty, std::string>(
      idp_array->name, array_of_strings, strings.size());
}

void CustomPropertiesExporter::write_idparray_of_numbers(IDProperty *idp_array)
{
  BLI_assert(idp_array->type == IDP_IDPARRAY);
  BLI_assert(idp_array->len > 0);

  /* This must be an array of arrays. */
  IDProperty *idp_rows = (IDProperty *)IDP_Array(idp_array);
  BLI_assert(idp_rows[0].type == IDP_ARRAY);

  const int subtype = idp_rows[0].subtype;
  if (!ELEM(subtype, IDP_INT, IDP_FLOAT, IDP_DOUBLE)) {
    /* Non-numerical types are not supported. */
    return;
  }

  switch (subtype) {
    case IDP_INT:
      static_assert(sizeof(int) == sizeof(int32_t), "Expecting 'int' to be 32-bit");
      write_idparray_flattened_typed<OInt32ArrayProperty, int32_t>(idp_array);
      break;
    case IDP_FLOAT:
      write_idparray_flattened_typed<OFloatArrayProperty, float>(idp_array);
      break;
    case IDP_DOUBLE:
      write_idparray_flattened_typed<ODoubleArrayProperty, double>(idp_array);
      break;
  }
}

template<typename ABCPropertyType, typename BlenderValueType>
void CustomPropertiesExporter::write_idparray_flattened_typed(IDProperty *idp_array)
{
  BLI_assert(idp_array->type == IDP_IDPARRAY);
  BLI_assert(idp_array->len > 0);

  const IDProperty *idp_rows = (IDProperty *)IDP_Array(idp_array);
  BLI_assert(idp_rows[0].type == IDP_ARRAY);
  BLI_assert(ELEM(idp_rows[0].subtype, IDP_INT, IDP_FLOAT, IDP_DOUBLE));

  const uint64_t num_rows = idp_array->len;
  std::vector<BlenderValueType> matrix_values;
  for (size_t row_idx = 0; row_idx < num_rows; ++row_idx) {
    const BlenderValueType *row = (BlenderValueType *)IDP_Array(&idp_rows[row_idx]);
    for (size_t col_idx = 0; col_idx < idp_rows[row_idx].len; col_idx++) {
      matrix_values.push_back(row[col_idx]);
    }
  }

  set_array_property<ABCPropertyType, BlenderValueType>(
      idp_array->name, &matrix_values[0], matrix_values.size());
}

template<typename ABCPropertyType, typename BlenderValueType>
void CustomPropertiesExporter::set_scalar_property(const StringRef property_name,
                                                   const BlenderValueType property_value)
{
  set_array_property<ABCPropertyType, BlenderValueType>(property_name, &property_value, 1);
}

template<typename ABCPropertyType, typename BlenderValueType>
void CustomPropertiesExporter::set_array_property(const StringRef property_name,
                                                  const BlenderValueType *array_values,
                                                  const size_t num_array_items)
{
  /* Create an Alembic property if it doesn't exist yet. */
  auto create_callback = [this, property_name]() -> OArrayProperty {
    ABCPropertyType abc_property(abc_compound_prop_, property_name);
    abc_property.setTimeSampling(timesample_index_);
    return abc_property;
  };
  OArrayProperty array_prop = abc_properties_.lookup_or_add_cb(property_name, create_callback);
  Alembic::Util::Dimensions array_dimensions(num_array_items);
  ArraySample sample(array_values, array_prop.getDataType(), array_dimensions);
  array_prop.set(sample);
}

}  // namespace blender::io::alembic
