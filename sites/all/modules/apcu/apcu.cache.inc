<?php
/**
 * @file
 * This integrates the APCu cache backend.
 */

require_once __DIR__ . '/vendor//symfony/polyfill-apcu/bootstrap.php';

/**
 * APC cache implementation.
 *
 * This is Drupal's APCu cache implementation. It uses Alternative PHP
 * User Cache to store cached data. Each cache bin corresponds to a prefix of
 * the apcu variables with the same name.
 *
 * This class is based on the original apc module:
 *
 *   http://drupal.org/project/apc
 *
 * It has been stripped down to work great with chained_fast module.
 */
class DrupalAPCuCache implements DrupalCacheInterface {
  /**
   * @var string
   */
  protected $bin;

  /**
   * @var string
   */
  protected $prefix;

  /**
   * Get prefix for bin using the configuration.
   *
   * @param string $bin
   *
   * @return string
   *   Can be an empty string, if no prefix set.
   */
  protected static function getPrefixSettingForBin($bin) {
    $prefixes = variable_get('cache_prefix', '');

    if (is_string($prefixes)) {
      // Variable can be a string, which then considered as a default behavior.
      return $prefixes;
    }

    if (isset($prefixes[$bin])) {
      if (FALSE !== $prefixes[$bin]) {
        // If entry is set and not FALSE, an explicit prefix is set for the bin.
        return $prefixes[$bin];
      } else {
        // If we have an explicit false, it means no prefix whatever is the
        // default configuration.
        return '';
      }
    } else {
      // Key is not set, we can safely rely on default behavior.
      if (isset($prefixes['default']) && FALSE !== $prefixes['default']) {
        return $prefixes['default'];
      } else {
        // When default is not set or an explicit FALSE, this means no prefix.
        return '';
      }
    }
  }

  function __construct($bin) {
    $this->bin = $bin;

    // First we determine the prefix from a setting.
    $prefix = self::getPrefixSettingForBin($this->bin);

    // If we do not have a configured prefix we use the HTTP_HOST.
    if (empty($prefix) && isset($_SERVER['HTTP_HOST'])) {
      // Provide a fallback for multisite. This is on purpose not inside the
      // getPrefixForBin() function in order to decouple the unified prefix
      // variable logic and custom module related security logic, that is not
      // necessary for all backends.
      $prefix = $_SERVER['HTTP_HOST'] . '::';
    } else {
      $prefix = $prefix . '::';
    }

    // When we are in testing mode we add the test prefix.
    if ($test_prefix = drupal_valid_test_ua()) {
      $prefix = $test_prefix . '::' . $prefix;
    }
    else if (isset($GLOBALS['drupal_test_info'])) {
      $prefix = $GLOBALS['drupal_test_info']['test_run_id'] . '::' . $prefix;
    }

    $this->prefix = $prefix;
  }

  /**
   * Function which retrieves the safe key for the cache bin.
   *
   * @return
   *   The safe APC key.
   */
  private function binKey() {
    return $this->prefix . $this->bin . '::';
  }

  /**
   * Function which retrieves the safe key for the cache cid.
   *
   * @param $cid
   *   The cache id.
   * @return
   *   The safe APC key.
   */
  private function key($cid) {
    return $this->binKey() . $cid;
  }

  function get($cid) {
    // Fetch the data.
    $cache = apcu_fetch($this->key($cid));
    return $this->prepareItem($cache);
  }

  /**
   * Prepare a cached item.
   *
   * Checks that items are either permanent or did not expire.
   *
   * @param $cache
   *   An item loaded from cache_get() or cache_get_multiple().
   * @return
   *   The item with data unserialized as appropriate or FALSE if there is no
   *   valid item to load.
   */
  protected function prepareItem($cache) {
    if (!isset($cache->data)) {
      return FALSE;
    }

    // If enforcing a minimum cache lifetime, validate that the data is
    // currently valid for this user before we return it by making sure the cache
    // entry was created before the timestamp in the current session's cache
    // timer. The cache variable is loaded into the $user object by _drupal_session_read()
    // in session.inc. If the data is permanent or we're not enforcing a minimum
    // cache lifetime always return the cached data.
    global $user;
    if ($cache->expire != CACHE_PERMANENT && variable_get('cache_lifetime', 0) && (isset($user->cache) && $user->cache > $cache->created)) {
      // This cache data is too old and thus not valid for us, ignore it.
      return FALSE;
    }

    return $cache;
  }

  function getMultiple(&$cids) {
    if (!$cids) {
      return array();
    }
    // We need to search the cache with the proper keys and
    // be able to get the original $cid back.
    foreach ($cids as $cid) {
      $keys[$this->key($cid)] = $cid;
    }

    $fetch = apcu_fetch(array_keys($keys));
    $cache = array();
    if (!empty($fetch)) {
        foreach ($fetch as $key => $data) {
          $item = $this->prepareItem($fetch[$key]);
          if (isset($item->data)) {
            $cache[$keys[$key]] = $item;
          }
        }
    }
    unset($fetch);

    $cids = array_diff($cids, array_keys($cache));

    return $cache;
  }

  function set($cid, $data, $expire = CACHE_PERMANENT, array $headers = NULL) {
    // Create new cache object.
    $cache = new stdClass();
    $cache->cid = $cid;
    // APC will serialize any structure we give itself.
    $cache->serialized = 0;
    $cache->created = REQUEST_TIME;
    $cache->created_microtime = round(microtime(TRUE), 3);
    $cache->expire = $expire;
    $cache->headers = isset($headers) ? $headers : NULL;

    $cache->data = $data;

    // What kind of expiration is being used.
    switch ($expire) {
      case CACHE_PERMANENT:
        $set_result = apcu_store($this->key($cid), $cache);
        break;

      case CACHE_TEMPORARY:
        if (variable_get('cache_lifetime', 0) > 0) {
          $set_result = apcu_store($this->key($cid), $cache, variable_get('cache_lifetime', 0));
        }
        else {
          $set_result = apcu_store($this->key($cid), $cache);
        }
        break;

      default:
        $set_result = apcu_store($this->key($cid), $cache, $expire - time());
        break;
    }
  }

  /**
   * Delete CID matching the given prefix.
   *
   * @param string $prefix
   */
  protected function deletePrefix($prefix) {
    if (class_exists('APCUIterator')) {
      $iterator = new APCUIterator('/^' . preg_quote($this->binKey() . $prefix, '/') . '/', APC_ITER_KEY);
      foreach ($iterator as $key => $data) {
        apcu_delete($key);
       }
    }
   }

  /**
   * Flush all cache items in a bin.
   */
  function flush() {
    $this->deletePrefix('');
  }

  function clear($cid = NULL, $wildcard = FALSE) {
    if (empty($cid)) {
      $this->flush();
    }
    else {
      if ($wildcard) {
        if ($cid == '*') {
          $this->flush();
        }
        else {
          $this->deletePrefix($cid);
        }
      }
      else if (is_array($cid)) {
        foreach ($cid as $entry) {
          apcu_delete($this->key($entry));
        }
      }
      else {
        apcu_delete($this->key($cid));
      }
    }
  }

  function isEmpty() {
    if (class_exists('APCUIterator')) {
      $iterator = new APCUIterator('/^' . preg_quote($this->binKey(), '/') . '/', APC_ITER_KEY);
      return 0 === $iterator->getTotalCount();
    }
    return TRUE;
  }

}
