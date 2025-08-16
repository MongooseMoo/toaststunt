require "test_helper"

class TestAstRoundTrip < Test::Unit::TestCase

  # Simple round-trip test parsing raw MOO response
  def test_round_trip(test_name, input_code)
    puts "\n=== #{test_name} ==="
    
    # Test the round-trip using raw command() response 
    result = command(%Q|; ast = parse_ast({"#{input_code}"}); return unparse_ast(ast);|)
    
    puts "Input:  #{input_code.inspect}"
    puts "Raw result: #{result.inspect}"
    
    # Parse the MOO result format: "{1, {"return 42;"}}" or "{1, {"line1", "line2"}}"
    assert(result.is_a?(String), "Expected string result from MOO, got: #{result.inspect}")
    assert(result.start_with?("{1, "), "Expected success result, got: #{result.inspect}")
    
    # Extract the code lines from the MOO format  
    if result =~ /\{1, \{(.+)\}\}$/
      code_part = $1
      # Handle single line: "return 42;"
      if code_part =~ /^\"(.+)\"$/
        unparsed_lines = [$1.gsub('\\"', '"')]
      # Handle multiple lines: "line1", "line2", "line3"  
      else
        # Split by quotes and commas, clean up
        unparsed_lines = code_part.split('", "').map { |line| 
          line.gsub(/^"/, '').gsub(/"$/, '').gsub('\\"', '"')
        }
      end
    else
      assert(false, "Could not parse MOO result format: #{result.inspect}")
    end
    
    puts "Parsed lines: #{unparsed_lines.inspect}"
    
    # Join lines for comparison
    unparsed_code = unparsed_lines.join("\n")
    
    # For simple expressions, should match exactly (except string literals which have different escaping)
    if input_code.include?("return") && !input_code.include?("if") && !input_code.include?("while") && !input_code.include?("\\\"")
      assert_equal(input_code, unparsed_code, "Round-trip failed for simple expression")
    end
    
    # Test that unparsed code is valid MOO syntax by trying to compile it
    begin
      o = create(:nothing)
      add_verb(o, [player, 'x', 'test'], ['this', 'none', 'this'])
      set_verb_code(o, 'test', unparsed_lines)
      puts "✅ Round-trip successful - unparsed code is valid"
    rescue => e
      assert(false, "Unparsed code is invalid MOO syntax: #{e.message}")
    ensure
      recycle(o) if o
    end
  end

  def test_basic_expressions
    run_test_as("wizard") do
      test_round_trip("Basic literal", "return 42;")
      test_round_trip("Arithmetic", "return 1 + 2;")
      test_round_trip("String literal", "return \\\"hello\\\";")
      test_round_trip("List literal", "return {1, 2, 3};")
    end
  end

  def test_simple_expression_round_trip
    run_test_as("wizard") do
      # Test arithmetic expression: 1 + 2
      puts "\n=== Testing arithmetic expression: 1 + 2 ==="
      arith_ast = command("; return parse_ast({\"return 1 + 2;\"});")
      puts "Arithmetic AST: #{arith_ast.inspect}"
      
      arith_unparsed = command("; ast = parse_ast({\"return 1 + 2;\"}); return unparse_ast(ast);")
      puts "Arithmetic unparsed: #{arith_unparsed.inspect}"
      
      # Test list literal expression
      puts "\n=== Testing list literal: {1, 2, 3} ==="
      list_ast = command("; return parse_ast({\"return {1, 2, 3};\"});")
      puts "List AST: #{list_ast.inspect}"
      
      list_unparsed = command("; ast = parse_ast({\"return {1, 2, 3};\"}); return unparse_ast(ast);")
      puts "List unparsed: #{list_unparsed.inspect}"
      
      # Test string literal
      puts "\n=== Testing string literal: \\\"hello\\\" ==="
      string_ast = command("; return parse_ast({\"return \\\"hello\\\";\"}); ")
      puts "String AST: #{string_ast.inspect}"
      
      string_unparsed = command("; ast = parse_ast({\"return \\\"hello\\\";\"}); return unparse_ast(ast);")
      puts "String unparsed: #{string_unparsed.inspect}"
    end
  end

  def test_comparison_equality
    run_test_as("wizard") do
      test_round_trip("Equality", "return 1 == 2;")
    end
  end

  def test_comparison_inequality 
    run_test_as("wizard") do
      test_round_trip("Inequality", "return 1 != 2;")
    end
  end

  def test_comparison_less_than
    run_test_as("wizard") do
      test_round_trip("Less than", "return 1 < 2;")
    end
  end

  def test_comparison_less_equal
    run_test_as("wizard") do
      test_round_trip("Less or equal", "return 1 <= 2;")
    end
  end

  def test_comparison_greater_than
    run_test_as("wizard") do
      test_round_trip("Greater than", "return 1 > 2;")
    end
  end

  def test_comparison_greater_equal
    run_test_as("wizard") do
      test_round_trip("Greater or equal", "return 1 >= 2;")
    end
  end

  # TDD PHASE 2 RED: Logical operators (expected to fail initially)
  def test_logical_and
    run_test_as("wizard") do
      test_round_trip("Logical AND", "return 1 && 2;")
    end
  end

  def test_logical_or
    run_test_as("wizard") do
      test_round_trip("Logical OR", "return 1 || 2;")
    end
  end

  def test_logical_not
    run_test_as("wizard") do
      test_round_trip("Logical NOT", "return !1;")
    end
  end

  # TDD PHASE 3 RED: Conditional expressions (expected to fail initially)
  def test_conditional_expression
    run_test_as("wizard") do
      test_round_trip("Conditional expression", "return 1 ? 2 | 3;")
    end
  end

  def test_conditional_expression_complex
    run_test_as("wizard") do
      test_round_trip("Complex conditional", "return (1 == 2) ? 42 | 0;")
    end
  end

  # TDD PHASE 4: Property and verb access
  def test_property_access
    run_test_as("wizard") do
      test_round_trip("Property access", "return player.name;")
    end
  end

  def test_verb_call_no_args
    run_test_as("wizard") do
      test_round_trip("Verb call no args", "return player:tell();")
    end
  end

  def test_verb_call_with_args
    run_test_as("wizard") do
      test_round_trip("Verb call with args", "return player:tell(42);")
    end
  end

  # TDD PHASE 5: Statement-level AST support
  def test_if_statement
    run_test_as("wizard") do
      test_round_trip("If statement", "if (1) return 42; endif")
    end
  end

  def test_if_else_statement
    run_test_as("wizard") do
      test_round_trip("If-else statement", "if (1) return 42; else return 0; endif")
    end
  end

  def test_while_loop
    run_test_as("wizard") do
      test_round_trip("While loop", "while (1) break; endwhile")
    end
  end

  def test_expression_statement
    run_test_as("wizard") do
      test_round_trip("Expression statement", "player:tell(42);")
    end
  end

end
