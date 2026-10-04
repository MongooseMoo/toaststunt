require 'test_helper'

class TestSpellcheck < Test::Unit::TestCase

  def setup
    run_test_as('wizard') do
      omit('the server was built without Aspell') if function_info('spellcheck') == E_INVARG
    end
  end

  def test_that_spellcheck_accepts_a_correct_word
    run_test_as('programmer') do
      assert_equal 1, simplify(command(%Q|; return spellcheck("hello");|))
    end
  end

  def test_that_spellcheck_suggests_for_a_misspelled_word
    run_test_as('programmer') do
      assert_equal 1, simplify(command(%Q|; return ("receive" in spellcheck("recieve")) && 1;|))
    end
  end

  def test_that_spellcheck_can_skip_suggestions
    run_test_as('programmer') do
      assert_equal 0, simplify(command(%Q|; return spellcheck("recieve", 0);|))
      assert_equal 1, simplify(command(%Q|; return spellcheck("receive", 0);|))
      assert_equal 1, simplify(command(%Q|; return ("receive" in spellcheck("recieve", 1)) && 1;|))
    end
  end

  # An Aspell speller keeps every suggestion list it has produced. 3,000
  # misspelled lookups used to leave about 30 MB behind for good.
  def test_that_suggestions_do_not_accumulate
    run_test_as('wizard') do
      lookups = %Q|for i in [1..1000] spellcheck("recieve"); spellcheck("definately"); spellcheck("mongoos"); endfor|
      resident_pages = %Q|; return toint(memory_usage()[2]);|
      simplify(command(%Q|; #{lookups} return 1;|))
      before = simplify(command(resident_pages))
      simplify(command(%Q|; #{lookups} return 1;|))
      after = simplify(command(resident_pages))
      assert after - before < 2048, "resident memory grew by #{after - before} pages over 3,000 misspelled lookups"
    end
  end

  def test_that_suggestions_survive_replacing_the_speller
    run_test_as('wizard') do
      assert_equal 1, simplify(command(%Q|; for i in [1..600] spellcheck("recieve"); endfor return ("receive" in spellcheck("recieve")) && 1;|))
    end
  end

end
