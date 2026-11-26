require 'test_helper'

# url_encode() and url_decode() only transform strings, so they work whether or
# not outbound networking is enabled. CI runs this file against a server started
# with -O; curl() itself must still refuse there.
class TestUrlBuiltins < Test::Unit::TestCase

  def test_that_url_encode_escapes_reserved_characters
    run_test_as('wizard') do
      assert_equal 'a%20b%2Fc%3Fd%3De', _('url_encode("a b/c?d=e")')
      assert_equal 'plain-text_1.2~', _('url_encode("plain-text_1.2~")')
    end
  end

  def test_that_url_decode_reverses_url_encode
    run_test_as('wizard') do
      assert_equal 'gamemaster/ui menu.m4a', _('url_decode("gamemaster%2Fui%20menu.m4a")')
      assert_equal 'a b/c?d=e', _('url_decode(url_encode("a b/c?d=e"))')
    end
  end

  def test_that_curl_still_refuses_when_outbound_networking_is_off
    run_test_as('wizard') do
      omit('curl() not compiled in') if _(%q|`function_info("curl") ! E_INVARG => 0'|) == 0
      assert_equal 'refused', _(%q|`curl("http://127.0.0.1:1/", ["timeout" -> 1]) ! E_PERM => "refused"'|)
    end
  end

end
