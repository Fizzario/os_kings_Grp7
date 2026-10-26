#!/bin/bash
# runs every line of parser_tests.txt through myshell and prints transcript
#
# the cases run in one directory and some of them depend on files the earlier
# ones made, so the fixture is laid down first and the leftovers are cleared
# afterwards, which keeps the transcript the same on every run

WORK=tests/scratch
rm -rf "$WORK" && mkdir -p "$WORK"
cp ./myshell "$WORK"/

# the input file the redirection and pipeline cases read from
printf 'beta\nalpha beta\nalpha\nbeta\n' > "$WORK"/in.txt

while IFS= read -r line; do
  printf '$ %s\n' "$line"
  printf '%s\n' "$line" | (cd "$WORK" && ./myshell)
done < tests/parser_tests.txt

rm -rf "$WORK"