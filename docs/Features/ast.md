# Syntax trees

Four built-in functions let MOO code read and write MOO code as data.

| Function | Returns |
| --- | --- |
| `parse_ast(LIST code)` | the syntax tree of `code`, a list of strings as given to `set_verb_code()` |
| `verb_ast(OBJ object, verb-desc)` | the syntax tree of a verb |
| `unparse_ast(LIST ast [, fully-paren [, indent]])` | the code for a syntax tree, as a list of strings |
| `validate_ast(ast)` | 1 if `unparse_ast()` would accept `ast`, otherwise 0 |

```
;parse_ast({"return x + 1;"})
=> {["type" -> "return", "value" -> ["type" -> "binary", "op" -> "+",
     "lhs" -> ["type" -> "variable", "name" -> "x"],
     "rhs" -> ["type" -> "literal", "value" -> 1]]]}

;unparse_ast({["type" -> "return", "value" -> ["type" -> "literal", "value" -> 1]]})
=> {"return 1;"}
```

## Behaviour

`parse_ast()` raises `E_TYPE` unless every element of `code` is a string. If
the code does not compile it raises `E_INVARG`; the message is the first
parser error and the value is the list of all of them, the same strings
`set_verb_code()` returns.

`verb_ast()` takes the arguments of `verb_code()` and raises the same errors:
`E_INVARG` for an invalid object, `E_VERBNF` if there is no such verb and
`E_PERM` if the caller may not read it.

The tree is the one `verb_code()` prints, so it describes what the server
compiled, not how the source was typed. Comments, extra parentheses and layout
are gone. `unparse_ast(parse_ast(code))` is `code` laid out the way
`verb_code()` would lay it out, and `unparse_ast(verb_ast(o, v))` equals
`verb_code(o, v)`.

`unparse_ast()` takes `fully-paren` and `indent` with the meaning they have
for `verb_code()`. It raises `E_INVARG` if the tree is malformed, with a
message that says where:

```
Invalid AST at [2].arms[1].condition.lhs: expected map, found string
```

It checks that the tree has a shape the parser could have produced. It does
not check the things the compiler checks, such as a `break` outside a loop;
`set_verb_code(o, v, unparse_ast(ast))` reports those. Fields it does not know
are ignored, so a tool can keep its own notes on a node. A tree may nest at
most 1000 levels deep.

None of these functions needs wizard permissions.

## Trees

A program is a list of statement nodes, and so is every block inside one. A
node is a map with a `"type"` string and the fields listed below. A field
marked with `?` is left out when the code has no such part and may be left out
of a tree you build. A name is a string holding a variable name.

### Statements

| `"type"` | Fields | Code |
| --- | --- | --- |
| `"expr"` | `expr` | `expr;` |
| `"return"` | `value`? | `return value;` |
| `"if"` | `arms`, `else`? | `if (...) ... elseif (...) ... else ... endif` |
| `"for"` | `var`, `index`?, `expr`, `body` | `for var, index in (expr) ... endfor` |
| `"for_range"` | `var`, `from`, `to`, `body` | `for var in [from..to] ... endfor` |
| `"while"` | `name`?, `condition`, `body` | `while name (condition) ... endwhile` |
| `"fork"` | `var`?, `delay`, `body` | `fork var (delay) ... endfork` |
| `"try_except"` | `body`, `excepts` | `try ... except var (codes) ... endtry` |
| `"try_finally"` | `body`, `finally` | `try ... finally ... endtry` |
| `"break"` | `name`? | `break name;` |
| `"continue"` | `name`? | `continue name;` |

`arms` is a list of maps `["condition" -> expr, "body" -> block]`, one for the
`if` and one for each `elseif`; it cannot be empty. `else`, `body` and
`finally` are blocks.

`excepts` is a list of maps `["var" -> name, "codes" -> list, "body" -> block]`
and cannot be empty. `var` is optional. `codes` is a list of expressions, as in
an argument list; leaving it out means `ANY`.

### Expressions

| `"type"` | Fields | Code |
| --- | --- | --- |
| `"literal"` | `value` | `17`, `1.5`, `"text"`, `#12`, `E_PERM` |
| `"variable"` | `name` | `name` |
| `"binary"` | `op`, `lhs`, `rhs` | `lhs op rhs` |
| `"unary"` | `op`, `expr` | `-expr`, `!expr`, `~expr` |
| `"assign"` | `target`, `value` | `target = value` |
| `"conditional"` | `condition`, `consequent`, `alternate` | `condition ? consequent \| alternate` |
| `"prop"` | `object`, `property` | `object.name`, `object.(property)` |
| `"verb_call"` | `object`, `verb`, `args` | `object:name(args)`, `object:(verb)(args)` |
| `"call"` | `function`, `args` | `function(args)` |
| `"index"` | `base`, `index` | `base[index]` |
| `"range"` | `base`, `from`, `to` | `base[from..to]` |
| `"first"` | | `^` inside an index or range |
| `"last"` | | `$` inside an index or range |
| `"list"` | `items` | `{items}` |
| `"map"` | `entries` | `[key -> value, ...]` |
| `"catch"` | `expr`, `codes`?, `default`? | `` `expr ! codes => default' `` |

`value` in a literal is the integer, float, string, object number or error
itself. `true` and `false` are variables. Negative numbers are literals.

`op` is one of `+ - * / % ^ && || == != < <= > >= in |. &. ^. << >>` for a
binary node and one of `- ! ~` for a unary one.

`property` and `verb` are expressions. `x.name` has a string literal there, and
`$name` is `#0.name`. `function` is the name of a built-in function as a
string.

`args`, `items` and `codes` are lists of expressions. In them, `@expr` is the
node `["type" -> "splice", "expr" -> expr]`, which is not valid anywhere else.

`entries` is a list of maps `["key" -> expr, "value" -> expr]`.

The `target` of an assignment is a variable, a property, or an index or range
of one of those. For a scattering assignment it is the node
`["type" -> "scatter", "items" -> list]`, where each item is a map
`["kind" -> kind, "var" -> name, "default" -> expr]`. `kind` is `"required"`,
`"optional"` or `"rest"`, and only an optional item can have a `default`.

## Example

Find every built-in function a verb calls:

```
calls = {};
todo = verb_ast(object, name);
while (todo)
  node = todo[1];
  todo = listdelete(todo, 1);
  if (typeof(node) == LIST)
    todo = {@node, @todo};
  elseif (typeof(node) == MAP)
    if (`node["type"] ! E_RANGE' == "call")
      calls = setadd(calls, node["function"]);
    endif
    todo = {@mapvalues(node), @todo};
  endif
endwhile
return calls;
```
