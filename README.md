# php-cfc

## Capture function call

```
   ___        ___  ___  __   ___
  / _ \/\  /\/ _ \/ __\/ _| / __\
 / /_)/ /_/ / /_)/ /  | |_ / /
/ ___/ __  / ___/ /___|  _/ /___
\/   \/ /_/\/   \____/|_| \____/

```

截获并统计PHP调用函数、方法的次数，用于长期项目迭代中产生的废弃代码清理

**优势** : 异步刷新Redis，几乎没有任何额外的性能损耗,支持多前缀匹配

### php.ini

```
cfc.enable=On
cfc.redis_host=127.0.0.1
cfc.redis_port=6379
cfc.prefix=App,Other
cfc.logfile=/tmp/cfc.log
cfc.ht_name=cfc_hash
```

### Dependencies

- hiredis
- pthread

### Tools

[cfc-viewer](https://github.com/imaben/cfc-viewer)
