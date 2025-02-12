<?php
/**
 * @file
 * Class representing a Salesforce SELECT SOQL query.
 */

class SalesforceSelectQuery {

  public $fields = array();
  public $order = array();
  public $objectType;
  public $limit;
  public $offset;
  public $conditions = array();

  /**
   * Constructor which sets the query object type.
   *
   * @param string $object_type
   *   Salesforce object type to query.
   */
  public function __construct($object_type = '') {
    $this->objectType = $object_type;
  }

  /**
   * Add a condition to the query.
   *
   * @param string $field
   *   Field name.
   * @param mixed $value
   *   Condition value. If an array, it will be split into quote enclosed
   *   strings separated by commas inside of parenthesis. Note that the caller
   *   must enclose the value in quotes as needed by the SF API.
   * @param string $operator
   *   Conditional operator. One of '=', '!=', '<', '>', 'LIKE, 'IN', 'NOT IN'.
   */
  public function addCondition($field, $value, $operator = '=') {
    if (is_array($value)) {
      $value = "('" . implode("','", $value) . "')";

      // Set operator to IN if wasn't already changed from the default.
      if ($operator == '=') {
        $operator = 'IN';
      }
    }

    $this->conditions[] = array(
      'field' => $field,
      'operator' => $operator,
      'value' => $value,
    );
  }

  /**
   * Implements PHP's magic toString().
   *
   * Function to convert the query to a string to pass to the SF API.
   *
   * @return string
   *   SOQL query ready to be executed the SF API.
   */
  // @codingStandardsIgnoreStart
  public function __toString() {

    $query = 'SELECT+';
    $query .= implode(',+', $this->fields);
    $query .= "+FROM+" . $this->objectType;

    if (count($this->conditions) > 0) {
      $where = array();
      foreach ($this->conditions as $condition) {
        $where[] = implode('+', $condition);
      }
      $query .= '+WHERE+' . implode('+AND+', $where);
    }

    if ($this->order) {
      $query .= "+ORDER+BY+";
      $fields = array();
      foreach ($this->order as $field => $direction) {
        $fields[] = $field . '+' . $direction;
      }
      $query .= implode(',+', $fields);
    }

    if ($this->limit) {
      $query .= "+LIMIT+" . (int) $this->limit;
    }

    if ($this->offset) {
      $query .= "+OFFSET+" . (int) $this->offset;
    }

    // Replaces spaces with + signs for custom filters.
    $query = str_replace(" ", "+", $query);

    return $query;
  }
  // @codingStandardsIgnoreEnd
}
