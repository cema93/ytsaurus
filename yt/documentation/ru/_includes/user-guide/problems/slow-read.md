# Медленное чтение таблиц и файлов

Схема перемещения данных при чтении:
![](../../../../images/read.svg)

## Как происходит чтение

Чанки таблицы лежат на дисках нодов в нативном формате, сжатые `compression_codec`. 
Оттуда они перемещаются на дата-прокси, где распаковываются, перекодирутся в заказанный пользователем формат (YSON, Protobuf или Skiff), далее как часть HTTP-ответа сжимаются указанным сетевым кодеком и попадают на машину пользователя.

## Что влияет на скорость чтения 

1. [**Медиум**](../../../user-guide/storage/media.md). Это вид дисков, на котором хранятся чанки таблицы. Находится в атрибуте `@primary_medium`. Скорость SSD, выше, чем HDD, но квота на них более ограничена. Чтобы поменять медиум, нужно сначала завести новую запись в атрибуте `@media` таблицы, например, `yt set //path/to/table/@media/ssd_blobs '{replication_factor=3}'`, а затем перенести таблицу в новый медиум: `yt set //path/to/table/@primary_medium ssd_blobs`  
1. [**Алгоритмы сжатия**](../../../user-guide/storage/compression.md) подразумевают компромисс между скоростью и качеством сжатия. Самый быстрый - `lz4`, медленнее и компактнее - от `brotli_1` до `brotli_11` (больше число - лучше сжатие). Нужно помнить, что есть два алгоритма:
   - Тот, с которым хранится таблица (`yt get //path/to/table/@compression_codec`). Для его изменения нужно установить атрибут `@compression_codec` и пережать все чанки таблицы (смотрите [команды](../../../user-guide/storage/compression.md#set_compression)).
   - Тот, который используется при пересылке по сети. Он зависит от используемого клиента.
     * В C++ API нужно выставлять опцию глобальной конфигурации `TConfig::Get()->AcceptEncoding`. Название алгоритмов из [раздела](../../../user-guide/storage/compression.md#compression_codecs) нужно префиксировать `"z-"`, например, `"z-snappy"`.
     * В Python API нужно выставить опцию `yt.wrapper.config["proxy"]["accept_encoding"]`. Поддерживаются алгоритмы `gzip`, `br` (brotli) и `identity`.
1. **Параллельность**. Если вы достигли ограничения на скорость чтения с дисков или форматы/компрессию на нодах, то может помочь параллельное чтение.
   - В Python API используйте секцию конфига `yt.wrapper.config["read_parallel"]`, там есть `"enable"`, `"data_size_per_thread"` и `"max_thread_count"`.
   - В C++ API нужно использовать функцию `CreateParallelTableReader` из особой библиотеки.

1. **Сеть**. Если данные лежат в одном дата-центре, а чтение производится из другого, то можно упереться в пропускную способность сети.
1. [**Формат**](../../../user-guide/storage/formats.md) Самый быстрый - skiff, далее идёт Protobuf, затем YSON.

1. **Нагрузка на дата-прокси**. Прокси разделены на группы (роли), по умолчанию все ходят через прокси с ролью `data`, поэтому такие прокси нагружены сильнее.
1. **Работы на кластере**. При обновлении узлов кластера, например, диски могут быть загружены сильнее.

## С чего начать?
1. Увеличьте параллельность (смотрите выше). Это может решить большинство проблем с чтением. Если это не помогло или недостаточно помогло, нужна дальнейшая диагностика.
1. Посмотрите в [профилировщике](https://en.wikipedia.org/wiki/Perf_(Linux)) или в [top-е](https://ru.wikipedia.org/wiki/Top) загрузку по CPU. Если видна деятельность, связанная с чтением, возможные причины:
   - HTTP-кодек. Например, Python API по умолчанию использует gzip, который может оказаться узким местом из-за скорости сжатия. В последнем случае в top-е может быть видно высокую загрузку CPU процессом python. Можно попробовать заменить на `identity`.
   -  Формат. Например, парсинг YSON и раскладывание его по объектам в памяти.
1. Попробуйте для эксперимента переложить часть данных на SSD (смотрите выше) и читать оттуда. Это позволит понять, упираетесь ли вы ограничение дисков нодов.
1. Если перечисленные шаги не помогли увеличить скорость, обратитесь к администратору системы. Если проблема связана с ограничением возможностей системы, напишите на рассылку  {{product-name}}  с описанием того, что вы предприняли, какие результаты получили и какие ожидали бы получить.