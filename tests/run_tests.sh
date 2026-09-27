# !/bin/bash
# runs every line of parser_tests.txt through myshell and prints transcript

while IFS= read -r line; do
  printf '$ %s\n' "$line"
  printf '%s\n' "$line" | ./myshell
done < tests/parser_tests.txt