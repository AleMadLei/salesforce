<?php

/**
 * @file
 * Data and callbacks for fieldmap types.
 */

/**
 * Implements hook_salesforce_mapping_fieldmap_type().
 */
function salesforce_mapping_salesforce_mapping_fieldmap_type() {
  $types = array(
    'property' => array(
      'label' => t('Properties'),
      'field_type' => 'select',
      'description' => t('Select a Backdrop field or property to map to a Salesforce field. Related are left out and should be handled using another fieldmap type like tokens.'),
      'options_callback' => 'salesforce_mapping_property_fieldmap_options',
      'push_value_callback' => 'salesforce_mapping_property_fieldmap_push_value',
      'pull_value_callback' => 'salesforce_mapping_property_fieldmap_pull_value',
      'validation_callback' => 'salesforce_mapping_property_validation',
    ),
    'token' => array(
      'label' => t('Token'),
      'description' => t('Enter a token to map a Salesforce field..'),
      'field_type' => 'textfield',
      'push_value_callback' => 'salesforce_mapping_token_fieldmap_value',
    ),
    'constant' => array(
      'label' => t('Constant'),
      'description' => t('Enter a constant value to map to a Salesforce field.'),
      'field_type' => 'textfield',
      'push_value_callback' => 'salesforce_mapping_constant_fieldmap_value',
    ),
    'related_entity' => array(
      'label' => t('Related entities'),
      'field_type' => 'select',
      'description' => t('If an existing connection is found with the selected entity, the linked identifiers will be used.'),
      'options_callback' => 'salesforce_mapping_related_entity_fieldmap_options',
      'push_value_callback' => 'salesforce_mapping_related_entity_fieldmap_push_value',
      'pull_value_callback' => 'salesforce_mapping_related_entity_fieldmap_pull_value',
    ),
  );

  if (module_exists('relation_endpoint')) {
    $types['relation_endpoints'] = array(
      'label' => t('Relation endpoints'),
      'field_type' => 'select',
      'description' => t('Allows multiple Salesforce fields to be selected as endpoint values.'),
      'options_callback' => 'salesforce_mapping_relation_endpoints_fieldmap_options',
      'pull_value_callback' => 'salesforce_mapping_relation_endpoints_fieldmap_pull_value',
      'salesforce_multiple_fields' => TRUE,
    );
  }

  return $types;
}

/**
 * Return all possible Backdrop properties for a given entity type.
 *
 * @param string $entity_type
 *   Name of entity whose properties to list.
 * @param bool $include_entities
 *   Choose whether or not to include properties of the given entity type that
 *   are themselves also entities.
 *
 * @return array
 *   List of entities that can be used as an #options list.
 */
function salesforce_mapping_property_fieldmap_options($entity_type, $entity_bundle = NULL, $include_entities = FALSE) {
  $options = array('' => t('-- Select --'));

  $properties = entity_plus_get_all_property_info($entity_type);
  if (isset($entity_bundle)) {
    $info = entity_plus_get_property_info($entity_type);
    $properties = $info['properties'];
    if (isset($info['bundles'][$entity_bundle])) {
      $properties += $info['bundles'][$entity_bundle]['properties'];
    }
  }
  else {
    $properties = entity_plus_get_all_property_info($entity_type);
  }

  foreach ($properties as $key => $property) {
    $type = isset($property['type']) ? entity_plus_property_extract_innermost_type($property['type']) : 'text';
    $is_entity = ($type == 'entity') || (bool) entity_get_info($type);
    // Leave entities out of this.
    if (($is_entity && $include_entities) || (!$is_entity && !$include_entities)) {
      if (isset($property['field']) && $property['field'] && !empty($property['property info'])) {
        foreach ($property['property info'] as $sub_key => $sub_prop) {
          $options[$property['label']][$key . ':' . $sub_key] = $sub_prop['label'];
          if (isset($sub_prop['required']) && $sub_prop['required']) {
            $options[$property['label']][$key . ':' . $sub_key] .= '*';
          }
        }
      }
      else {
        $options[$key] = $property['label'];
        if (isset($property['required']) && $property['required']) {
          $options[$key] .= '*';
        }
      }
    }
  }

  return $options;
}

/**
 * Return all possible Backdrop properties of type entity for a given entity type.
 *
 * @param string $entity_type
 *   The type of entity of whose related entities to find.
 *
 * @return array
 *   The list of related entities that can be used as an #options list.
 */
function salesforce_mapping_related_entity_fieldmap_options($entity_type, $entity_bundle) {
  return salesforce_mapping_property_fieldmap_options($entity_type, $entity_bundle, TRUE);
}

/**
 * Value callback for related entity fieldmap type.
 *
 * @param array $fieldmap
 *   Map of Backdrop field to Salesforce field associations.
 * @param object $entity_wrapper
 *   Entity wrapper object.
 *
 * @return mixed
 *   The value to push.
 */
function salesforce_mapping_related_entity_fieldmap_push_value($fieldmap, $entity_wrapper) {
  $value = NULL;
  if (property_exists($entity_wrapper->{$fieldmap['backdrop_field']['fieldmap_value']}, 'isEntityList')) {
    $related_wrapper = $entity_wrapper->{$fieldmap['backdrop_field']['fieldmap_value']}->get(0);
  }
  else {
    $related_wrapper = $entity_wrapper->{$fieldmap['backdrop_field']['fieldmap_value']};
  }
  if ($related_entity = $related_wrapper->value()) {
    list($related_entity_id) = entity_extract_ids($related_wrapper->type(), $related_entity);
    $mapping_object = salesforce_mapping_object_load_by_backdrop(
      $related_wrapper->type(),
      $related_entity_id
    );
    if ($mapping_object) {
      $value = $mapping_object->salesforce_id;
    }
  }
  return $value;
}

/**
 * Pull value callback for property fieldmap type.
 */
function salesforce_mapping_related_entity_fieldmap_pull_value($entity_wrapper, $sf_object, $field_map) {
  $mapping_object = salesforce_mapping_object_load_by_sfid($sf_object[$field_map['salesforce_field']['name']]);
  $info = $entity_wrapper->info();
  if ($mapping_object && !isset($info['relation_type'])) {
    $entity = entity_load($mapping_object->entity_type, $mapping_object->entity_id);
    $entity = reset($entity);
    return entity_metadata_wrapper($mapping_object->entity_type, $entity);
  }
  // Handle relations.
  elseif (module_exists('relation') && isset($info['relation_type'])) {
    // We cannot create relationships between new items. We are saving them here
    // to avoid performing a duplicate save for all entities in
    // salesforce_pull_process_records().
    if (!$info['parent']->getIdentifier()) {
      $info['parent']->save();
    }

    list($parent_entity_id) = entity_extract_ids($info['parent']->type(), $info['parent']->value());
    $parent_type = $info['parent']->type();
    $query = relation_query($parent_type, $parent_entity_id);
    $query->propertyCondition('relation_type', $info['relation_type']);
    $results = $query->execute();
    if ($results) {
      $relations = relation_load_multiple(array_keys($results));
      $match = TRUE;
    }
    else {
      $match = FALSE;
    }

    if (isset($relations)) {
      foreach ($relations as $rid => $relation) {
        $match = TRUE;
        // Check if matches existing.
        $current_endpoints = $relations[$rid]->endpoints;
        if (
          ($current_endpoints[LANGUAGE_NONE][0]['entity_type'] != $parent_type) ||
          ($current_endpoints[LANGUAGE_NONE][0]['entity_id'] != $parent_entity_id) ||
          ($current_endpoints[LANGUAGE_NONE][1]['entity_type'] != $mapping_object->entity_type) ||
          ($current_endpoints[LANGUAGE_NONE][1]['entity_id'] != $mapping_object->entity_id)
        ) {
          $match = FALSE;
        }
        // If same type and not a match delete relation.
        if ($current_endpoints[LANGUAGE_NONE][1]['entity_type'] == $mapping_object->entity_type && !$match) {
          relation_delete($rid);
        }
      }
    }
    // Create new relation and endpoints.
    if ($mapping_object && !$match) {
      $relation_type = $info['relation_type'];
      $endpoints = array(
        array(
          'entity_type' => $parent_type,
          'entity_id' => $parent_entity_id,
        ),
        array(
          'entity_type' => $mapping_object->entity_type,
          'entity_id' => $mapping_object->entity_id,
        ),
      );
      $relation_new = relation_create($relation_type, $endpoints);
      relation_save($relation_new);
    }
  }

  return NULL;
}

/**
 * Return relation endpoint property as an option.
 *
 * @param string $entity_type
 *   The type of entity of whose to find relation endpoints of.
 *
 * @return array
 *   List of relation endpoints that can be used in an #options array, or FALSE
 *   if none are found.
 */
function salesforce_mapping_relation_endpoints_fieldmap_options($entity_type, $entity_bundle) {
  $properties = entity_plus_get_all_property_info($entity_type);
  if (isset($properties['endpoints'])) {
    return array('endpoints' => $properties['endpoints']['label']);
  }
  else {
    return FALSE;
  }
}

/**
 * Pull value callback for relation endpoint fieldmap type.
 *
 * @param EntityMetadataWrapper $entity_wrapper
 *   Wrapper for the related entity being synced.
 * @param SalesforceMappingObject $sf_object
 *   Mapped Backdrop and Salesforce objects.
 * @param array $field_map
 *   Field mapping data.
 *
 * @return array
 *   Array of Backdrop entities.
 */
function salesforce_mapping_relation_endpoints_fieldmap_pull_value($entity_wrapper, $sf_object, $field_map) {
  $values = array();

  // Load each related Backdrop entity.
  foreach ($field_map['salesforce_field'] as $sf_field) {
    $mapping_object = salesforce_mapping_object_load_by_sfid($sf_object[$sf_field['name']]);
    if ($mapping_object) {
      $entity = entity_load($mapping_object->entity_type, $mapping_object->entity_id);
      $entity = reset($entity);
      //$entity = entity_plus_load_single($mapping_object->entity_type, $mapping_object->entity_id);
      $values[] = entity_metadata_wrapper($mapping_object->entity_type, $entity);
    }
  }

  // Load the relation type entity.
  $relation_type_name = $entity_wrapper->info();
  $relation_type = relation_type_load($relation_type_name['parent']->getBundle());

  // If the relationship is directional, ensure source/bundle are in the right
  // order.
  $return_values = array();
  if ($relation_type->directional) {
    foreach ($values as $value) {
      foreach ($relation_type->source_bundles as $bundle_key) {
        list($source_entity_type) = explode(':', $bundle_key);
        if ($source_entity_type == $value->type()) {
          $return_values[0] = $value;
          break;
        }
      }

      foreach ($relation_type->target_bundles as $bundle_key) {
        list($target_entity_type) = explode(':', $bundle_key);
        if ($target_entity_type == $value->type()) {
          $return_values[1] = $value;
          break;
        }
      }
    }
  }

  return empty($return_values) ? $values : $return_values;
}

/**
 * Value callback for property fieldmap type.
 *
 * @param array $fieldmap
 *   Map of Backdrop field to Salesforce field associations.
 * @param object $entity_wrapper
 *   Entity wrapper object.
 *
 * @return mixed
 *   The push value.
 */
function salesforce_mapping_property_fieldmap_push_value($fieldmap, $entity_wrapper) {
  $backdrop_fields_array = explode(':', $fieldmap['backdrop_field']['fieldmap_value']);
  $parent = $entity_wrapper;
  foreach ($backdrop_fields_array as $backdrop_field) {
    if ($parent instanceof EntityListWrapper) {
      // First list<> types, get the property from the first item.
      $child_wrapper = $parent->get(0)->{$backdrop_field};
    }
    else {
      $child_wrapper = $parent->{$backdrop_field};
    }
    $parent = $child_wrapper;
  }
  $value = NULL;
  try {
    $value = $child_wrapper->value();
  }
  catch (EntityMetadataWrapperException $e) {
    // Don't throw an exception, if we can't get the value, it probably means
    // the field is empty.
    // @TODO: Find a better way to check the validity of child wrappers.
  }

  // Salesforce wants arrays to be imploded strings.
  if (is_array($value)) {
    $value = implode(SALESFORCE_MAPPING_ARRAY_DELIMITER, $value);
  }

  // Field type specific handling.
  if ($child_wrapper->type() == 'date' && isset($value) && ($fieldmap['salesforce_field']['type'] == 'date' || $fieldmap['salesforce_field']['type'] == 'datetime')) {
    // Backdrop provides a timestamp, Salesforce wants ISO 8601 formatted
    // date/time.
    $value = format_date($value, 'custom', 'c');
  }

  // Boolean SF fields only want real boolean values. NULL is also not allowed.
  if ($fieldmap['salesforce_field']['type'] == 'boolean') {
    $value = (bool) $value;
  }

  return $value;
}

/**
 * Pull value callback for property fieldmap type.
 */
function salesforce_mapping_property_fieldmap_pull_value($entity_wrapper, $sf_object, $field_map) {
  // Check to see if the value is set in Salesforce.
  $value = NULL;
  if (isset($sf_object[$field_map['salesforce_field']['name']])) {
    $value = $sf_object[$field_map['salesforce_field']['name']];
  }

  if (isset($value)) {
    // Salesforce provides multipicklist values as a delimited string. If the
    // destination field in Backdrop accepts multiple values, explode the string
    // into an array.
    if ($entity_wrapper instanceof EntityListWrapper) {
      $value = explode(SALESFORCE_MAPPING_ARRAY_DELIMITER, $value);
    }

    // Marshalling for data types that are formatted differently on Salesforce.
    switch ($entity_wrapper->type()) {
      case 'date':
        if ($field_map['salesforce_field']['type'] == 'date' || $field_map['salesforce_field']['type'] == 'datetime') {
          $value = strtotime($value);
        }
        break;

      case 'integer':
      case 'boolean':
        $value = (int) $value;
        break;

      case 'text':
        $value = (string) $value;
        break;

      case 'uri':
        if (!empty($value) && !valid_url($value, TRUE)) {
          $parsed_value = parse_url($value);
          // Salesforce tacks on an http:// to the front of URLs as necessary,
          // but it still passes the naked version to the API, so we correct it.
          if (!isset($parsed_value['scheme'])) {
            $value = 'http://' . $value;
          }
        }

        break;
    }
  }

  return $value;
}

/**
 * Property field validation callback.
 *
 * @TODO: Add length validation.
 */
function salesforce_mapping_property_validation($backdrop_field, $sf_field, $direction, $key) {
  $valid = FALSE;
  $length_valid = TRUE;

  // Type validation
  // direction[array Salesforce type][array Backdrop type]
  $verify_array = array(
    'sync' => array(
      'string' => array('text', 'list<text>', 'uri'),
      'int' => array('integer'),
      'id' => array('text'),
      'datetime' => array('date'),
      'email' => array('text'),
    ),
    'backdrop_sf' => array(
      // Allow any source field for string type.
      'string' => array(),
      'int' => array('integer'),
      'id' => array('text'),
      'datetime' => array('date'),
      'email' => array('text'),
    ),
    'sf_backdrop' => array(
      'string' => array('text', 'int', 'list<text>'),
      'int' => array('integer'),
      'id' => array('text'),
      'datetime' => array('date'),
      'email' => array('text'),
    ),
  );

  $backdrop_type = isset($backdrop_field['type']) ? $backdrop_field['type'] : 'text';
  $sf_type_array = isset($verify_array[$direction][$sf_field['type']]) ? $verify_array[$direction][$sf_field['type']] : array();
  if (empty($sf_type_array)) {
    // If type is not found in validation array, assume valid.
    return TRUE;
  }

  $type_valid = (array_search($backdrop_type, $sf_type_array) === FALSE) ? FALSE : TRUE;
  $valid = $type_valid && $length_valid;
  if (!$valid) {
    form_set_error(
      "salesforce_field_mappings][{$key}][salesforce_field",
      t('Backdrop field %backdrop_label is of type %backdrop_type, Salesforce field %sf_label is of type %sf_type and cannot be mapped in the %dir direction',
        array(
          '%backdrop_label' => $backdrop_field['label'],
          '%backdrop_type' => $backdrop_type,
          '%sf_label' => $sf_field['label'],
          '%sf_type' => $sf_field['type'],
          '%dir' => $direction,
        )
      ));
  }

  // Ensure that a property has a setter method if it needs to be updated.
  if (in_array($direction, array(SALESFORCE_MAPPING_DIRECTION_SYNC, SALESFORCE_MAPPING_DIRECTION_SF_BACKDROP))) {
    if (empty($backdrop_field['setter callback'])) {
      form_set_error(
        "salesforce_field_mappings][{$key}][salesforce_field",
        t('Backdrop field %backdrop_label does not have a setter method and cannot be written to.',
          array('%backdrop_label' => $backdrop_field['label'])
        ));
    }
  }
}

/**
 * Value callback for token fieldmap type.
 *
 * @param array $fieldmap
 *   Map of Backdrop field to Salesforce field associations.
 * @param object $entity_wrapper
 *   Entity wrapper object.
 *
 * @return mixed
 *   Token value.
 */
function salesforce_mapping_token_fieldmap_value($fieldmap, $entity_wrapper) {
  $token = $fieldmap['backdrop_field']['fieldmap_value'];
  $value = token_replace($token, array(
    $entity_wrapper->type() => $entity_wrapper->value(),
  ), array('clear' => TRUE));
  return $value;
}

/**
 * Value callback for constant fieldmap type.
 *
 * @param array $fieldmap
 *   Map of Backdrop field to Salesforce field associations.
 * @param object $entity_wrapper
 *   Entity wrapper object.
 *
 * @return mixed
 *   Constant value.
 */
function salesforce_mapping_constant_fieldmap_value($fieldmap, $entity_wrapper) {
  return $fieldmap['backdrop_field']['fieldmap_value'];
}
