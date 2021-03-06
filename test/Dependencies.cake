(set-cakelisp-option cakelisp-src-dir "src")
(c-import "<stdio.h>")
(import "DependenciesModule.cake")

(defun main (&return int)
  (printf "%s\n" (hello-from-comptime))
  (empty-macro)
  (test-generator)
  (cond (a (return 1))
        (b (return 2))
        (c (return 3))
        (true (return 4)))
  (cond (true (return 1)))
  (return 0))

(defmacro hello-from-comptime ()
  (var startToken (& (const Token)) (at startTokenIndex tokens))
  (output.push_back (array TokenType_String "Hello, world!"
                           startToken.source startToken.lineNumber
                           startToken.columnStart startToken.columnEnd))
  (return true))

(defmacro amazing-print-create ()
  (var startToken (& (const Token)) (at startTokenIndex tokens))
  ;; I need quoting soon lol
  (output.push_back (array TokenType_OpenParen ""
                           startToken.source startToken.lineNumber
                           startToken.columnStart startToken.columnEnd))
  (output.push_back (array TokenType_Symbol "defun"
                           startToken.source startToken.lineNumber
                           startToken.columnStart startToken.columnEnd))
  (output.push_back (array TokenType_Symbol "amazing-print"
                           startToken.source startToken.lineNumber
                           startToken.columnStart startToken.columnEnd))
  (output.push_back (array TokenType_OpenParen ""
                           startToken.source startToken.lineNumber
                           startToken.columnStart startToken.columnEnd))
  (output.push_back (array TokenType_CloseParen ""
                           startToken.source startToken.lineNumber
                           startToken.columnStart startToken.columnEnd))
  (output.push_back (array TokenType_OpenParen ""
                           startToken.source startToken.lineNumber
                           startToken.columnStart startToken.columnEnd))
  (output.push_back (array TokenType_Symbol "printf"
                           startToken.source startToken.lineNumber
                           startToken.columnStart startToken.columnEnd))
  (output.push_back (array TokenType_String "It worked"
                           startToken.source startToken.lineNumber
                           startToken.columnStart startToken.columnEnd))
  (output.push_back (array TokenType_CloseParen ""
                           startToken.source startToken.lineNumber
                           startToken.columnStart startToken.columnEnd))
  (output.push_back (array TokenType_CloseParen ""
                           startToken.source startToken.lineNumber
                           startToken.columnStart startToken.columnEnd))
  ;; (bad-function)
  (return true))

(amazing-print-create)

(defgenerator test-generator ()
  (var startToken (& (const Token)) (at startTokenIndex tokens))

  (addStringOutput output.source "printf(\"This is a test\");"
                   StringOutMod_NewlineAfter (addr startToken))
  (return true))

(defstruct-local test-struct
  a ([] 5 int)
  b (* (const char))
  c bool
  tokens (& (<> std::vector Token)))

(defstruct object-reference
  tokens (* (const (<> std::vector Token)))
  start-index int
  context EvaluatorContext
  splice-output (* GeneratorOutput)
  is-resolved bool)
