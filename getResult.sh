#!/usr/bin/env sh

i=0
while [ $((i)) -lt 25 ]; do
  str=""
  if [ "$i" -lt 10 ]; then
    str="0$i"
  else
    str="$i"
  fi

  gcc ./test/func.c "./test/test$str.c"
  ./a.out
  printf "\n"

  i=$((i+1))
done
