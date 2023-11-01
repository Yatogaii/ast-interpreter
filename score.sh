#!/usr/bin/env sh

set -- "100" "10" "20" "200" "10" "10" "20" "10" "20" "20" "5" "100" "4" "20" "12" "-8" "30" "10" "1020" "1020" "5" "33312826232118161311863491419242934" "2442" "2442" "2442"

i=0
while [ $((i)) -lt 25 ]; do
  str=""
  if [ "$i" -lt 10 ]; then
    str="0$i"
  else
    str="$i"
  fi
  i=$((i + 1))

  printf "%s\t" "$str"
  if output="$(/root/ast-interpreter/build/ast-interpreter "$(cat "/root/ast-interpreter/test/test$str.c")" 2>&1)"; then
    answer="$(eval "echo \${${i}}")"
    printf "%s\t$s\t" "$output" "$answer"
    if [ "$output" = "$answer" ]; then
      echo 1
    else
      echo -1
    fi
  else
    echo 0
  fi
done
