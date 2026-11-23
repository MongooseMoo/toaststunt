require 'test_helper'

# sqlite-vec is compiled into the server and registered for every connection
# sqlite_open() makes: its functions and vec0 tables need no loading step.
class TestSqliteVec < Test::Unit::TestCase
  def moo(code)
    simplify(command("; #{code}"))
  end

  def test_functions_are_available_on_a_new_connection
    run_test_as('wizard') do
      result = moo(<<~MOO.tr("\n", ' '))
        h = sqlite_open(":memory:");
        r = sqlite_query(h, "SELECT vec_version(), vec_distance_l2('[1, 2]', '[4, 6]'), vec_length('[1, 2, 3]');");
        sqlite_close(h);
        return r;
      MOO
      assert_equal ['v0.1.9', 5.0, 3], result
    end
  end

  def test_vec0_table_answers_nearest_neighbour_queries
    run_test_as('wizard') do
      result = moo(<<~MOO.tr("\n", ' '))
        h = sqlite_open(":memory:");
        sqlite_query(h, "CREATE VIRTUAL TABLE v USING vec0(e float[2]);");
        sqlite_execute(h, "INSERT INTO v(rowid, e) VALUES (?, ?), (?, ?), (?, ?);", {1, "[0, 0]", 2, "[3, 4]", 3, "[1, 0]"});
        r = sqlite_execute(h, "SELECT rowid, distance FROM v WHERE e MATCH ? AND k = 2 ORDER BY distance;", {"[0, 0]"});
        sqlite_close(h);
        return r;
      MOO
      assert_equal [[1, 0.0], [3, 1.0]], result
    end
  end
end
