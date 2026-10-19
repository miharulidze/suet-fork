set pagination off
set confirm off
set breakpoint pending on
break suet_ses_cq_write
commands
silent
set $e=(unsigned long*)$rsi
printf "SUET_CQ context=%p flags=%#lx len=%lu buf=%p tag=%#lx\n", $e[0], $e[1], $e[2], $e[3], $e[5]
if ($e[1] & 1024) && $e[3] && $e[2] >= 32
x/4gx $e[3]
else
if ($e[1] & 1024) && $e[3] && $e[2] >= 8
x/1gx $e[3]
end
end
continue
end
run -b 8 -e 8 -f 2 -g 2 -w 0 -n 1 -c 1
