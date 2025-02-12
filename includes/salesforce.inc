<?php
/**
 * @file
 * Objects, properties, and methods to communicate with the Salesforce REST API
 */

/**
 * Ability to authorize and communicate with the Salesforce REST API.
 */
class Salesforce {

  public $response;

  /**
   * Constructor which initializes the consumer.
   *
   * @param string $consumer_key
   *   Salesforce key to connect to your Salesforce instance.
   * @param string $consumer_secret
   *   Salesforce secret to connect to your Salesforce instance.
   */
  public function __construct($consumer_key, $consumer_secret = '') {
    $config = config('salesforce.settings');
    $auth_config = config('salesforce.auth_settings');
    $this->consumer_key = $consumer_key;
    $this->consumer_secret = $consumer_secret;
    $this->login_url = $auth_config->get('salesforce_endpoint');

    $this->rest_api_version = $config->get('salesforce_api_version');
  }

  /**
   * Converts a 15-character case-sensitive Salesforce ID to 18-character
   * case-insensitive ID. If input is not 15-characters, return input unaltered.
   *
   * @param string $sfid15
   *   15-character case-sensitive Salesforce ID
   *
   * @return string
   *   18-character case-insensitive Salesforce ID
   */
  public static function convertId($sfid15) {
    if (strlen($sfid15) != 15) {
      return $sfid15;
    }
    $chunks = str_split($sfid15, 5);
    $extra = '';
    foreach ($chunks as $chunk) {
      $chars = str_split($chunk, 1);
      $bits = '';
      foreach ($chars as $char) {
        $bits .= (!is_numeric($char) && $char == strtoupper($char)) ? '1' : '0';
      }
      $map = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ012345';
      $extra .= substr($map, base_convert(strrev($bits), 2, 10), 1);
    }
    return $sfid15 . $extra;
  }

  /**
   * Given a Salesforce ID, return the corresponding SObject name. (Based on
   *  keyPrefix from object definition, @param string $sfid
   *   15- or 18-character Salesforce ID
   *
   * @return string
   *   sObject name, e.g. "Account", "Contact", "my__Custom_Object__c" or FALSE
   *   if no match could be found.
   * @throws SalesforceException
   * @see
   *  https://developer.salesforce.com/forums/?id=906F0000000901ZIAQ )
   *
   */
  public function getSobjectType($sfid) {
    $objects = $this->objects(['keyPrefix' => substr($sfid, 0, 3)]);
    if (count($objects) == 1) {
      // keyPrefix is unique across objects. If there is exactly one return value from objects(), then we have a match.
      $object = reset($objects);
      return $object['name'];
    }
    // Otherwise, we did not find a match.
    return FALSE;
  }

  /**
   * Determine if this SF instance is fully configured.
   *
   * @TODO: Consider making a test API call.
   */
  public function isAuthorized() {
    return !empty($this->consumer_key) && !empty($this->consumer_secret) && $this->getRefreshToken();
  }

  /**
   * Make a call to the Salesforce REST API.
   *
   * @param string $path
   *   Path to resource.
   * @param array $params
   *   Parameters to provide.
   * @param string $method
   *   Method to initiate the call, such as GET or POST.  Defaults to GET.
   * @param string $type
   *   Type of object to request. Defaults to 'rest'. Most common examples:
   *   - rest - Call the standard Salesforce REST API.
   *   - apexrest - Call a custom REST URL defined in Salesforce.
   *
   * @return mixed
   *   The requested response.
   *
   * @throws SalesforceException
   */
  public function apiCall($path, $params = [], $method = 'GET', $type = 'rest') {
    if (!$this->getAccessToken()) {
      $this->refreshToken();
    }

    $this->response = $this->apiHttpRequest($path, $params, $method, $type);

    switch ($this->response->code) {
      // The session ID or OAuth token used has expired or is invalid.
      case 401:
        // Refresh token.
        $this->refreshToken();

        // Rebuild our request and repeat request.
        $this->response = $this->apiHttpRequest($path, $params, $method, $type);
        // Throw an error if we still have bad response.
        if (!in_array($this->response->code, [200, 201, 204])) {
          throw new SalesforceException($this->response->error, $this->response->code);
        }

        break;

      case 200:
      case 201:
      case 204:
        // All clear.
        break;

      default:
        // We have problem and no specific Salesforce error provided.
        if (empty($this->response->data)) {
          throw new SalesforceException($this->response->error, $this->response->code);
        }
    }

    $data = backdrop_json_decode($this->response->data);

    if (empty($data[0])) {
      $this->checkResponseDataForApiError($data);
    }
    else {
      foreach ($data as $item) {
        $this->checkResponseDataForApiError($item);
      }

      if (count($data) == 1) {
        $data = $data[0];
      }
    }

    return $data;
  }

  /**
   * Private helper that throws an exception if a API error is found in the
   * response data.
   *
   * @param array $data
   *   Array of salesforce api response data.
   *
   * @throws SalesforceException
   */
  private function checkResponseDataForApiError($data) {
    if (isset($data['error'])) {
      throw new SalesforceException($data['error_description'], $data['error']);
    }

    if (!empty($data['errorCode'])) {
      throw new SalesforceException($data['message'], $this->response->code);
    }

  }

  /**
   * Private helper to issue an SF API request.
   *
   * @param string $path
   *   Path to resource.
   * @param array $params
   *   Parameters to provide.
   * @param string $method
   *   Method to initiate the call, such as GET or POST.  Defaults to GET.
   * @param string $type
   *   Type of request. Examples include:
   *   - rest (default)
   *   - apexrest (custom REST written in APEX in Salesforce instance)
   *
   * @return object
   *   The requested data.
   */
  protected function apiHttpRequest($path, $params, $method, $type = 'rest') {
    $url = $this->getApiEndPoint($type) . $path;
    $headers = [
      'Authorization' => 'OAuth ' . $this->getAccessToken(),
      'Content-type' => 'application/json',
    ];
    $data = backdrop_json_encode($params);
    return $this->httpRequest($url, $data, $headers, $method);
  }

  /**
   * Make the HTTP request. Wrapper around backdrop_http_request().
   *
   * @param string $url
   *   Path to make request from.
   * @param array $data
   *   The request body.
   * @param array $headers
   *   Request headers to send as name => value.
   * @param string $method
   *   Method to initiate the call, such as GET or POST.  Defaults to GET.
   *
   * @return object
   *   Salesforce response object.
   */
  protected function httpRequest($url, $data, $headers = [], $method = 'GET') {

    // If method is GET, data should not be sent otherwise backdrop_http_request()
    // will set Content-Length header which confuses some proxies like Squid.
    if ($method === 'GET') {
      $data = NULL;
    }

    // Build the request, including path and headers. Internal use.
    $options = [
      'method' => $method,
      'headers' => $headers,
      'data' => $data,
    ];

    return backdrop_http_request($url, $options);
  }

  /**
   * Get the API end point for a given type of the API.
   *
   * @param string $api_type
   *   E.g., rest, partner, enterprise.
   *
   * @return string
   *   Complete URL endpoint for API access.
   */
  public function getApiEndPoint($api_type = 'rest') {
    $url = &backdrop_static(__FUNCTION__ . $api_type);
    if (!isset($url)) {
      // Special handling for apexrest, since it's not in the identity object.
      if ($api_type == 'apexrest') {
        $url = $this->getInstanceUrl() . '/services/apexrest/';
      }
      else {
        $identity = $this->getIdentity();
        $url = str_replace('{version}', $this->rest_api_version['version'], $identity['urls'][$api_type]);
      }
    }
    return $url;
  }

  /**
   * Get the SF instance URL. Useful for linking to objects.
   */
  public function getInstanceUrl() {
    return config_get('salesforce.auth_settings', 'salesforce_instance_url');
  }

  /**
   * Set the SF instanc URL.
   *
   * @param string $url
   *   URL to set.
   */
  protected function setInstanceUrl($url) {
    config_set('salesforce.auth_settings', 'salesforce_instance_url', $url);
  }

  /**
   * Get the access token.
   */
  public function getAccessToken() {
    return config_get('salesforce.auth_settings', 'salesforce_access_token');
  }

  /**
   * Set the access token.
   *
   * It is stored in session.
   *
   * @param string $token
   *   Access token from Salesforce.
   */
  protected function setAccessToken($token) {
    config_set('salesforce.auth_settings', 'salesforce_access_token', $token);
  }

  /**
   * Get refresh token.
   */
  protected function getRefreshToken() {
    return config_get('salesforce.auth_settings', 'salesforce_refresh_token');
  }

  /**
   * Set refresh token.
   *
   * @param string $token
   *   Refresh token from Salesforce.
   */
  protected function setRefreshToken($token) {
    config_set('salesforce.auth_settings', 'salesforce_refresh_token', $token);
  }

  /**
   * Refresh access token based on the refresh token. Updates session variable.
   *
   * @throws SalesforceException
   */
  public function refreshToken() {
    $refresh_token = $this->getRefreshToken();
    if (empty($refresh_token)) {
      throw new SalesforceException(t('There is no refresh token.'));
    }

    $data = backdrop_http_build_query([
      'grant_type' => 'refresh_token',
      'refresh_token' => $refresh_token,
      'client_id' => $this->consumer_key,
      'client_secret' => $this->consumer_secret,
    ]);
    $url = $this->login_url . '/services/oauth2/token';
    $headers = [
      // This is an undocumented requirement on Salesforce's end.
      'Content-Type' => 'application/x-www-form-urlencoded',
    ];
    $response = $this->httpRequest($url, $data, $headers, 'POST');

    if ($response->code != 200) {
      // @TODO: Deal with error better.
      throw new SalesforceException(t('Unable to get a Salesforce access token.'), $response->code);
    }

    $data = backdrop_json_decode($response->data);

    $this->checkResponseDataForApiError($data);

    $this->setAccessToken($data['access_token']);
    $this->setIdentity($data['id']);
    $this->setInstanceUrl($data['instance_url']);
  }

  /**
   * Retrieve and store the Salesforce identity given an ID url.
   *
   * @param string $id
   *   Identity URL.
   *
   * @throws SalesforceException
   */
  protected function setIdentity($id) {
    $headers = [
      'Authorization' => 'OAuth ' . $this->getAccessToken(),
      'Content-type' => 'application/json',
    ];
    $response = $this->httpRequest($id, NULL, $headers);
    if ($response->code != 200) {
      throw new SalesforceException(t('Unable to access identity service.'), $response->code);
    }
    $data = backdrop_json_decode($response->data);
    config_set('salesforce.auth_settings', 'salesforce_identity', $data);
  }

  /**
   * Return the Salesforce identity, which is stored in a variable.
   *
   * @return array
   *   Returns FALSE is no identity has been stored.
   *
   * @throws SalesforceException
   */
  public function getIdentity() {
    $config = config('salesforce.auth_settings');
    $identity = $config->get('salesforce_identity');
    if ($identity) {
      $id_url_scheme = parse_url($identity['id']);
      $allowed_endpoint = $config->get('salesforce_endpoint');
      $allowed_endpoint_url_scheme = parse_url($allowed_endpoint);
      if ($id_url_scheme['host'] != $allowed_endpoint_url_scheme['host']) {
        throw new SalesforceException(t('Salesforce identity does not match salesforce endpoint: you need to re-authenticate.'));
      }
    }
    return $identity;
  }

  /**
   * OAuth step 1: Redirect to Salesforce and request and authorization code.
   */
  public function getAuthorizationCode() {
    $url = $this->login_url . '/services/oauth2/authorize';
    $query = [
      'redirect_uri' => $this->redirectUrl(),
      'response_type' => 'code',
      'client_id' => $this->consumer_key,
    ];

    backdrop_goto($url, ['query' => $query]);
  }

  /**
   * OAuth step 2: Exchange an authorization code for an access token.
   *
   * @param string $code
   *   Code from Salesforce.
   */
  public function requestToken($code) {
    $data = backdrop_http_build_query([
      'code' => $code,
      'grant_type' => 'authorization_code',
      'client_id' => $this->consumer_key,
      'client_secret' => $this->consumer_secret,
      'redirect_uri' => $this->redirectUrl(),
    ]);

    $url = $this->login_url . '/services/oauth2/token';
    $headers = [
      // This is an undocumented requirement on SF's end.
      'Content-Type' => 'application/x-www-form-urlencoded',
    ];
    $response = $this->httpRequest($url, $data, $headers, 'POST');

    $data = backdrop_json_decode($response->data);

    if ($response->code != 200) {
      $error = isset($data['error_description']) ? $data['error_description'] : $response->error;
      throw new SalesforceException($error, $response->code);
    }

    // Ensure all required attributes are returned. They can be omitted if the
    // OAUTH scope is inadequate.
    $required = ['refresh_token', 'access_token', 'id', 'instance_url'];
    foreach ($required as $key) {
      if (!isset($data[$key])) {
        return FALSE;
      }
    }

    $this->setRefreshToken($data['refresh_token']);
    $this->setAccessToken($data['access_token']);
    $this->setIdentity($data['id']);
    $this->setInstanceUrl($data['instance_url']);

    return TRUE;
  }

  /**
   * Helper to build the redirect URL for OAUTH workflow.
   *
   * @return string
   *   Redirect URL.
   */
  protected function redirectUrl() {
    return url('salesforce/oauth_callback', [
      'absolute' => TRUE,
      'language' => (object) ['language' => FALSE],
      'https' => TRUE,
    ]);
  }

  /**
   * @defgroup salesforce_apicalls Wrapper calls around core apiCall()
   */

  /**
   * Available objects and their metadata for your organization's data.
   *
   * @param array $conditions
   *   Associative array of filters to apply to the returned objects. Filters
   *   are applied after the list is returned from Salesforce.
   * @param bool $reset
   *   Whether to reset the cache and retrieve a fresh version from Salesforce.
   *
   * @return array
   *   Available objects and metadata.
   *
   * @addtogroup salesforce_apicalls
   */
  public function objects($conditions = ['updateable' => TRUE], $reset = FALSE) {
    $cache = cache_get('salesforce_objects');
    // Force the recreation of the cache when it is older than 5 minutes.
    if ($cache && REQUEST_TIME < ($cache->created + 300) && !$reset) {
      $result = $cache->data;
    }
    else {
      $result = $this->apiCall('sobjects');
      // Allow the cache to clear at any time by not setting an expire time.
      cache_set('salesforce_objects', $result, 'cache', CACHE_TEMPORARY);
    }

    if (!empty($conditions)) {
      foreach ($result['sobjects'] as $key => $object) {
        foreach ($conditions as $condition => $value) {
          if ($object[$condition] != $value) {
            unset($result['sobjects'][$key]);
          }
        }
      }
    }

    return $result['sobjects'];
  }

  /**
   * Use SOQL to get objects based on query string.
   *
   * @param SalesforceSelectQuery $query
   *   The constructed SOQL query.
   *
   * @return array
   *   Array of Salesforce objects that match the query.
   *
   * @addtogroup salesforce_apicalls
   */
  public function query(SalesforceSelectQuery $query) {
    backdrop_alter('salesforce_query', $query);
    // Casting $query as a string calls SalesforceSelectQuery::__toString().
    $result = $this->apiCall('query?q=' . (string) $query);
    return $result;
  }

  /**
   * Same as ::query() but also returns deleted or archived records.
   *
   * @param SalesforceSelectQuery $query
   *   The constructed SOQL query.
   *
   * @return array
   *   Array of Salesforce objects that match the query.
   *
   * @addtogroup salesforce_apicalls
   */
  public function queryAll(SalesforceSelectQuery $query) {
    backdrop_alter('salesforce_query', $query);
    // Casting $query as a string calls SalesforceSelectQuery::__toString().
    $result = $this->apiCall('queryAll?q=' . (string) $query);
    return $result;
  }

  /**
   * Retreieve all the metadata for an object.
   *
   * @param string $name
   *   Object type name, E.g., Contact, Account, etc.
   * @param bool $reset
   *   Whether to reset the cache and retrieve a fresh version from Salesforce.
   *
   * @return array
   *   All the metadata for an object, including information about each field,
   *   URLs, and child relationships.
   *
   * @addtogroup salesforce_apicalls
   */
  public function objectDescribe($name, $reset = FALSE) {
    if (empty($name)) {
      return [];
    }
    $cache = cache_get($name, 'cache_salesforce_object');
    // Force the recreation of the cache when it is older than 5 minutes.
    if ($cache && REQUEST_TIME < ($cache->created + 300) && !$reset) {
      return $cache->data;
    }
    else {
      $object = $this->apiCall("sobjects/{$name}/describe");
      // Sort field properties, because salesforce API always provides them in a
      // random order. We sort them so that stored and exported data are
      // standardized and predictable.
      $fields = [];
      foreach ($object['fields'] as $field) {
        ksort($field);
        if (!empty($field['picklistValues'])) {
          foreach ($field['picklistValues'] as &$picklist_value) {
            ksort($picklist_value);
          }
        }
        $fields[$field['name']] = $field;
      }
      ksort($fields);
      $object['fields'] = $fields;
      // Allow the cache to clear at any time by not setting an expire time.
      cache_set($name, $object, 'cache_salesforce_object', CACHE_TEMPORARY);
      return $object;
    }
  }

  /**
   * Create a new object of the given type.
   *
   * @param string $name
   *   Object type name, E.g., Contact, Account, etc.
   * @param array $params
   *   Values of the fields to set for the object.
   *
   * @return array
   *   "id" : "001D000000IqhSLIAZ",
   *   "errors" : [ ],
   *   "success" : true
   *
   * @addtogroup salesforce_apicalls
   */
  public function objectCreate($name, $params) {
    return $this->apiCall("sobjects/{$name}", $params, 'POST');
  }

  /**
   * Create new records or update existing records.
   *
   * The new records or updated records are based on the value of the specified
   * field.  If the value is not unique, REST API returns a 300 response with
   * the list of matching records.
   *
   * @param string $name
   *   Object type name, E.g., Contact, Account.
   * @param string $key
   *   The field to check if this record should be created or updated.
   * @param string $value
   *   The value for this record of the field specified for $key.
   * @param array $params
   *   Values of the fields to set for the object.
   *
   * @return array
   *   success:
   *     "id" : "00190000001pPvHAAU",
   *     "errors" : [ ],
   *     "success" : true
   *   error:
   *     "message" : "The requested resource does not exist"
   *     "errorCode" : "NOT_FOUND"
   *
   * @addtogroup salesforce_apicalls
   */
  public function objectUpsert($name, $key, $value, $params) {
    // If key is set, remove from $params to avoid UPSERT errors.
    if (isset($params[$key])) {
      unset($params[$key]);
    }

    $data = $this->apiCall("sobjects/{$name}/{$key}/{$value}", $params, 'PATCH');
    if ($this->response->code == 300) {
      $data['message'] = t('The value provided is not unique.');
    }
    return $data;
  }

  /**
   * Update an existing object.
   *
   * @param string $name
   *   Object type name, E.g., Contact, Account.
   * @param string $id
   *   Salesforce id of the object.
   * @param array $params
   *   Values of the fields to set for the object.
   *
   * @addtogroup salesforce_apicalls
   */
  public function objectUpdate($name, $id, $params) {
    $this->apiCall("sobjects/{$name}/{$id}", $params, 'PATCH');
  }

  /**
   * Return a full loaded Salesforce object.
   *
   * @param string $name
   *   Object type name, E.g., Contact, Account.
   * @param string $id
   *   Salesforce id of the object.
   *
   * @return object
   *   Object of the requested Salesforce object.
   *
   * @addtogroup salesforce_apicalls
   */
  public function objectRead($name, $id) {
    return $this->apiCall("sobjects/{$name}/{$id}", [], 'GET');
  }

  /**
   * Return a full loaded Salesforce object from External ID.
   *
   * @param string $name
   *   Object type name, E.g., Contact, Account.
   * @param string $field
   *   Salesforce external id field name.
   * @param string $value
   *   Value of external id.
   *
   * @return object
   *   Object of the requested Salesforce object.
   *
   * @addtogroup salesforce_apicalls
   */
  public function objectReadbyExternalId($name, $field, $value) {
    return $this->apiCall("sobjects/{$name}/{$field}/{$value}");
  }

  /**
   * Delete a Salesforce object.
   *
   * @param string $name
   *   Object type name, E.g., Contact, Account.
   * @param string $id
   *   Salesforce id of the object.
   *
   * @addtogroup salesforce_apicalls
   */
  public function objectDelete($name, $id) {
    $this->apiCall("sobjects/{$name}/{$id}", [], 'DELETE');
  }

  /**
   * Retrieves the list of individual objects that have been deleted within the
   * given timespan for a specified object type.
   *
   * @param string $type
   *   Object type name, E.g., Contact, Account.
   * @param string $startDate
   *   Start date to check for deleted objects (in ISO 8601 format).
   * @param string $endDate
   *   End date to check for deleted objects (in ISO 8601 format).
   *
   * @return GetDeletedResult
   */
  public function getDeleted($type, $startDate, $endDate) {
    return $this->apiCall("sobjects/{$type}/deleted/?start={$startDate}&end={$endDate}");
  }

  /**
   * Return a list of available resources for the configured API version.
   *
   * @return array
   *   Associative array keyed by name with a URI value.
   *
   * @addtogroup salesforce_apicalls
   */
  public function listResources() {
    $resources = $this->apiCall('');
    foreach ($resources as $key => $path) {
      $items[$key] = $path;
    }
    return $items;
  }

  /**
   * Return a list of SFIDs for the given object, which have been created or
   * updated in the given timeframe.
   *
   * @param string $name
   *   Object type name, E.g., Contact, Account.
   *
   * @param int $start
   *   unix timestamp for older timeframe for updates.
   *   Defaults to "-29 days" if empty.
   *
   * @param int $end
   *   unix timestamp for end of timeframe for updates.
   *   Defaults to now if empty
   *
   * @return array
   *   return array has 2 indexes:
   *     "ids": a list of SFIDs of those records which have been created or
   *       updated in the given timeframe.
   *     "latestDateCovered": ISO 8601 format timestamp (UTC) of the last date
   *       covered in the request.
   *
   * @see https://developer.salesforce.com/docs/atlas.en-us.api_rest.meta/api_rest/resources_getupdated.htm
   *
   * @addtogroup salesforce_apicalls
   */
  public function getUpdated($name, $start = NULL, $end = NULL) {
    if (empty($start)) {
      $start = strtotime('-29 days');
    }
    $start = urlencode(gmdate(DATE_ATOM, $start));

    if (empty($end)) {
      $end = time();
    }
    $end = urlencode(gmdate(DATE_ATOM, $end));

    return $this->apiCall("sobjects/{$name}/updated/?start=$start&end=$end");
  }

  /**
   * Given a DeveloperName and SObject Name, return the SFID of the
   * corresponding RecordType. DeveloperName doesn't change between Salesforce
   * environments, so it's safer to rely on compared to SFID.
   *
   * @param string $name
   *   Object type name, E.g., Contact, Account.
   *
   * @param string $devname
   *   RecordType DeveloperName, e.g. Donation, Membership, etc.
   *
   * @return string SFID
   *   The Salesforce ID of the given Record Type, or null.
   */
  public function getRecordTypeIdByDeveloperName($name, $devname, $reset = FALSE) {
    $cache = cache_get('salesforce_record_types');
    // Force the recreation of the cache when it is older than 5 minutes.
    if ($cache && REQUEST_TIME < ($cache->created + 300) && !$reset) {
      return !empty($cache->data[$name][$devname])
        ? $cache->data[$name][$devname]['Id']
        : NULL;
    }
    $query = new SalesforceSelectQuery('RecordType');
    $query->fields = ['Id', 'Name', 'DeveloperName', 'SobjectType'];
    $result = $this->query($query);
    $record_types = [];
    foreach ($result['records'] as $rt) {
      $record_types[$rt['SobjectType']][$rt['DeveloperName']] = $rt;
    }
    cache_set('salesforce_record_types', $record_types, 'cache', CACHE_TEMPORARY);
    return !empty($record_types[$name][$devname])
      ? $record_types[$name][$devname]['Id']
      : NULL;
  }

}

class SalesforceException extends Exception { }

class SalesforcePullException extends SalesforceException { }
