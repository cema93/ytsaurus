# Map

Map операция может состоять из одного или нескольких джобов. На вход каждому джобу поступает часть данных из входных таблиц, которые обрабатываются пользовательским скриптом. Результат записывается в выходную таблицу.

У операции Map есть два режима: **unordered** (по умолчанию) и **ordered**. Включение режима ordered Map регулируется настройкой `ordered=%true` в спецификации операции. Режим влияет на гарантии, которые система предоставляет относительно входных данных для джобов. Любой из двух режимов соблюдает базовую гарантию операции Map: каждая выходная таблица составляется как объединение результатов всех успешно завершившихся джобов, при этом каждая строка из входных данных окажется на входе ровно одного успешно завершившегося джоба.

- В режиме unordered Map нет **никаких** гарантий сверх описанной выше. В частности:
  - Строки одной входной таблицы могут попадать в произвольные джобы без каких-либо свойств непрерывности; в частности, один маппер может получить первую и третью строку таблицы, а другой вторую и четвёртую.
  - Строки одной входной таблицы могут попадать в джоб в произвольном порядке, не обязательно в порядке возрастания номера строки.
  - Строки разных входных таблиц могут попадать в один джоб в произвольном порядке, в том числе вперемешку; то есть сначала может следовать строка таблицы A, потом таблицы B, и потом снова строка таблицы A.

- Ordered Map работает следующим образом:
  - Конкатенирует входные данные в порядке их указания в секции input_table_paths входных данных.
  - Получившийся совокупный вход разделяет на непрерывные отрезки, каждый из которых подаётся на вход отдельному джобу.
  - Если входных таблиц несколько, то некоторые джобы **могут** получить на вход строки из нескольких таблиц, если он попал на границу между двумя таблицами в описанной выше конкатенации.
  - Каждый джоб читает свой отрезок данных строго по порядку в описанной выше конкатенации, - сначала по порядку следования таблиц, а внутри одной таблицы по возрастанию номеров строк.
  
Данные режимы реализуют компромисс: режим ordered Map даёт больше гарантий, которыми можно пользоваться в отдельных случаях, но менее эффективен, так как накладывает больше ограничений. Режим unordered Map активно пользуется возможностью адаптивно читать входные данные с большой степенью параллельности из разных чанков, компенсируя замедления отдельных дисков со входными данными, поэтому в общем случае более эффективен. Режим unordered Map может произвольным образом перегруппировывать строки в пуле ещё не обработанных входных данных, в частности, вход джоба, завершившегося неудачно (aborted или failed), может быть произвольным образом перегруппирован и обработан в нескольких новых джобах.

Обратите внимание, опция `ordered=%true` регулирует способ разбиения входных таблиц на джобы, но **не обеспечивает** никакой дополнительной сортировки лежащих в них данных кроме той, которая была изначально. 

Порядок следования выходных данных определяется как режимом операции, так и наличием на выходной таблице сортированной схемы (или атрибута `sorted_by` на пути к ней в спецификации операции). 

Возможны следующие сценарии:

- Если выходная таблица сортированная, то выполняется следующее:
  - Выходные данные каждого джоба проверяются на сортированность относительно схемы выходной таблицы, либо указанной на пути выходной таблицы. Если джоб вывел не сортированную последовательность строк, то операция завершается ошибкой.
  - Если все выходные чанки сортированные, то в конце работы операции они «пытаются» встать в соответствии с порядком сортировки на выходной таблице. Если оказывается, что два чанка перекрывают друг друга по граничным ключам и при любом варианте взаимного расположения их строки не образуют сортированную последовательность, операция завершается ошибкой. Чанки, состоящие из единственного ключа, упорядочиваются между собой произвольным недетерминированным образом.
- Если выходная таблица не сортированная, а режим операции unordered, то выходные чанки упорядочиваются между собой произвольным недетерминированным образом.
- Если выходная таблица не сортированная, а режим операции ordered, то выходные чанки упорядочиваются в соответствии с описанным выше глобальным порядком следования джобов.

Ordered-версия Map операции обладает свойством детерминированности набора и порядка строк выходной таблицы при условии детерминированности логики мапперов вне зависимости от числа джобов в операции и порядка их выполнения. Это свойство не выполнено для unordered Map

Общие параметры для всех типов операций описаны в разделе [Настройки операций](../../../user-guide/data-processing/operations/operations-options.md).

У Map операции поддерживаются следующие дополнительные параметры (в скобках указаны значения по умолчанию, если заданы):

* `mapper` — пользовательский скрипт;
* `input_table_paths` — список входных таблиц с указанием полных путей (не может быть пустым);
* `output_table_paths` — список выходных таблиц;
* `job_count`, `data_size_per_job` (256 Mb) — опции, которые указывают сколько джобов должно быть запущено, имеют рекомендательный характер. Опция `job_count` имеет приоритет над `data_size_per_job`. Если указать `job_count` меньшим, либо равным `total_input_row_count`, то гарантируется, что количество джобов будет точно соблюдено, если это не противоречит ограничению на максимальное число джобов в операции. В частности, если `job_count` равен `total_input_row_count`, то в каждый джоб попадёт ровно одна строка, если `job_count` равен единице, то будет запущен ровно один джоб;
* `ordered` — управляет способом разбиения входа по джобам;
* `auto_merge` — словарь, содержащий настройки автоматического объединения выходных чанков небольшого размера. По умолчанию автоматическое слияние отключено.

## Пример спецификации

```yaml
{
  pool = "my_cool_pool";
  job_count = 100;
  input_table_paths = [ "//tmp/input_table" ];
  output_table_paths = [ "//tmp/output_table" ];
  mapper = {
    command = "cat";
    memory_limit = 1000000;
  }
}
```
