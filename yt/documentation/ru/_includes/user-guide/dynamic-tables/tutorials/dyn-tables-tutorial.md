# Создание веб-сервиса

В данном разделе описан пример создания простого веб-сервиса с использованием динамических таблиц.

В рамках примера будет построен бекэнд для небольшого сервиса хранения и показа комментариев. Можно рассматривать его как упрощенный аналог [Reddit](https://www.reddit.com).
Для этого будет разработан stateless сервис c HTTP-интерфейсом, работающий поверх консистентного, отказоустойчивого и масштабируемого хранилища, в котором будут находиться топики, комментарии, лайки.

Используемые технологии и инструменты:

- Исходный код сервиса на Python 3.4 и выше.
- В качестве хранилища будут использоваться [реплицированные динамические таблицы](../../../user-guide/dynamic-tables/replicated-dynamic-tables.md) {{product-name}} в режиме синхронной репликации. Работа с динамическими таблицами реализована через RPC прокси.
- В качестве основы для HTTP API будет использоваться фреймворк [Flask](http://flask.pocoo.org/).

{% note info "Примечание" %}

Данный пример не предусматривает разработку таких компонент как: авторизация, rate limiting, сервисные мониторинги.
Описываемый пример предназначен в первую очередь для знакомства с принципами использования и возможностями динамических таблиц {{product-name}}, а не для разработки production high-load веб-сервисов на Python.

{% endnote %}

Чтобы скачать примеры кода, перейдите по [ссылке](https://github.com/ytsaurus/ytsaurus/tree/main/yt/examples/comment_service).

##  Проектирование { #designing }

Требования к сервису:

- Все комментарии, хранимые в сервисе, должны относиться к одному из топиков.
- Комментарии внутри топика должны быть организованы в дерево, у каждого комментария, кроме корневого комментария топика, должен быть один родитель.
- Каждый комментарий должен характеризоваться строкой `parent_path`. Она представляет собой путь из идентификаторов предков данного комментария в дереве.
- Для каждого комментария должно храниться имя пользователя, количество просмотров, время создания.
- API должно позволять:
  - создавать, редактировать и удалять комментарии;
  - выбирать все комментарии по топику или поддерево комментариев внутри топика;
  - выбирать последние комментарии, оставленные данным пользователем;
  - выбирать последние добавленные топики.

Ожидаемые характеристики:

* до 50 000 комментариев внутри топика;
* типичный размер комментария — не больше десятков тысяч символов;
* по нагрузке (RPS) и объему данных сервис должен горизонтально масштабироваться.

### Методы API { #api }

В случае POST-запросов параметры запросов передаются в теле запроса в виде JSON-документа. В GET-запросах — в URL. Во всех случаях результат запроса или ошибка приходит в теле ответа в виде JSON-документа. Возможные HTTP-коды ответа:

| HTTP код | Описание |
| -------------- | --------------- |
| 200 | GET/POST запрос был успешно выполнен. |
| 201 | Значение было успешно добавлено. |
| 400 | Некорректный запрос. Например, не заданы некоторые обязательные параметры. |
| 404 | Объект или объекты по заданному критерию не найдены. Например, попытка добавить комментарий в несуществующий топик. |
| 409 | Конфликт при внесении изменений в динамическую таблицу. |
| 503 | В данный момент сервис не может обработать запрос. Например, мета-кластер обновляется и/или динамические таблицы отмонтированы.  |
| 524 | Превышен таймаут при работе с динамическими таблицами. |

В случае любой ошибки с кодами 4xx и 5xx в теле ответа приходит JSON-документ вида `{"error" : "message"}`.

Запись данных — добавление, изменение, удаление комментария — будет реализовано через `POST`, чтение — через `GET`. В качестве идентификатора комментария используется `guid` — такой подход позволит обеспечивать равномерную нагрузку на шарды динамических таблиц.

#### Сигнатуры вызовов { #call_signatures }

- `POST: /post_comment` — добавление комментария.

  | Имя параметра | Тип | Обязательный параметр | Описание |
  | -------- | ------ | -------------- | --------------- |
  | `topic_id` | `guid` | Нет | Идентификатор топика, если не задан — создается новый топик. |
  | `parent_path` | `string` | Нет | Путь к комментарию-родителю. Может быть не задан, если не указан `topic_id`. |
  | `content` | `string` | Да | Содержимое комментария. |
  | `user` | `string` | Да | Имя пользователя. |

В случае успешного выполнения возвращается код 201 и документ вида `{"comment_id" : "<guid>", "new_topic" : True | False} `;

- `POST: /edit_comment` — редактирование комментария.

  | Имя параметра | Тип | Обязательный параметр | Описание |
  | --- | --- | --- | --- |
  | `topic_id` | `guid` | Да | Идентификатор топика. |
  | `parent_path` | `string` | Да | Путь к комментарию. |
  | `content` | `string` | Да | Новое содержимое комментария. |

  В случае успешного выполнения возвращается код 200 и пустой документ.

- `POST: /delete_comment` — удаление комментария. При этом из базы данных комментарий в действительности не удаляется, чтобы не нарушать древовидную структуру комментариев. Вместо этого в параметрах комментария выставляется специальный флаг о том, что он удален, после чего комментарий больше нельзя редактировать.

  | Имя параметра | Тип | Обязательный параметр |Описание |
  | --- | --- | --- | --- |
  | `topic_id` | `guid` | Да | Идентификатор топика. |
  | `parent_path` | `string` | Да | Путь к комментарию. |

  В случае успешного выполнения возвращается код 200 и пустой документ.

- `GET: /topic_comments` — получение комментария в топике. Для каждого комментария хранится счетчик просмотров, который увеличивается на единицу каждый раз при запросе комментариев топика.

  | Имя параметра | Тип | Обязательный параметр | Описание |
  | --- | --- | --- | --- |
  | `topic_id` | `guid` | Да | Идентификатор топика. |
  | `parent_path` | `string` | Нет | Путь к корневому комментарию — может быть указан, если требуется получить только поддерево комментариев по заданному топику. |

  В случае успешного выполнения возвращается код 200 и документ со списком комментариев следующего вида:

    ```json
    [
      {
        "comment_id":"<id>",
        "parent_id":"<id>",
        "content":"<large_text>",
        "user":"<user_name>",
        "view_count":"<N>",
        "update_time":"<UTC_time>",
        "deleted":"True|False"
      }
    ]
    [{"comment_id" : "<id>", "parent_id" : "<id>", "content" : "<large_text>", "user" : "<user_name>", "view_count" : <N>, "update_time" : "<UTC_time>", "deleted" : True|False}]
    ```

  Возвращаемые комментарии сортируются по `parent_path`, поэтому ответы на комментарий оказываются расположены сразу после него и при этом упорядочиваются по времени создания.

* `GET: /last_user_comments` — получить список комментариев пользователя, отсортированный в обратном хронологическом порядке. Предполагается, что в веб-интерфейсе сервиса будет страница, где пользователь сможет посмотреть все свои комментарии.

  | Имя параметра | Тип | Обязательный параметр |Описание |
  | --- | --- | --- | --- |
  | `user` | `string` | Да | Имя пользователя. |
  | `limit` | `int` | Нет | Ограничение числа возвращаемых комментариев. |
  | `from_time` | `UTC_time` | Нет | Возвращать только комментарии с `update_time` больше данного параметра. Специально используется параметр `from_time` вместо `offset`, потому что язык запросов {{product-name}} не поддерживает ключевое слово `OFFSET` в запросах. |

  В случае успешного выполнения возвращается код 200 и документ со списком комментариев следующего вида:

    ```json
    [{"comment_id" : "<id>", "topic_id" : "<id>", "content" : "<large_text>", "user" : <user_name>, "view_count" : <N>, "update_time" : <UTC_time>}]
    ```

* `GET: /last_topics` — получить топики, в которых недавно обновлялись комментарии. Топиком является корневой комментарий. Предполагается, что данный запрос будет использоваться главной страницей сервиса, список будет общим для всех пользователей. Необходима поддержка пагинации.

  | Имя параметра | Тип | Обязательный параметр |Описание |
  | --- | --- | --- | --- |
  | `limit` | `int` | Нет | Ограничение на число возвращаемых топиков. |
  | `from_time` | `UTC_time` | Нет | Возвращать только комментарии с `update_time` больше данного параметра. Параметр `from_time` используется вместо `offset` намеренно, потому что язык запросов {{product-name}} не поддерживает ключевое слово `OFFSET` в запросах. |

  В случае успешного выполнения возвращается код 200 и документ со списком комментариев следующего вида:

    ```json
    [{"topic_id" : "<id>", "content" : "<large_text>", "user" = <user_name>, "view_count" : <N>, "update_time" : <UTC_time>}]
    ```

###  Описание таблиц с данными { #tables_description }

Нужно определить необходимый набор таблиц и их схему. При проектировании схемы стоит учитывать возможности и ограничения динамических таблиц:

- В каждой динамической таблице есть уникальный первичный ключ. По первичному ключу можно быстро получать записи вызовом команды `lookup`.
- Динамические таблицы поддерживают транзакции — в том числе между разными таблицами в модели `snapshot isolation`.
- Динамические таблицы не поддерживают вторичные индексы. Эффективный поиск или фильтрация возможны только по префиксу первичного ключа с помощью механизма вывода диапазонов. В противном случае запрос может выродиться в full scan таблицы.
- Горизонтальное масштабирование динамических таблиц осуществляется посредством шардирования. Шард таблицы в системе называется `tablet`. Он задается непрерывным диапазоном значений первичного ключа. Чтобы шардирование было эффективно, необходимо чтобы чтение и запись были равномерно распределены между таблетами.
- Вторичные индексы можно эмулировать, создавая дополнительные таблицы по соответствующему первичному ключу. В таком случае запись в основную таблицу и таблицу-индекс необходимо осуществлять в одной транзакции.

Необходима основная таблица, в которой будет храниться контент — `topic_comments`. Запросы на чтение к такой таблице в основном будут ограничены одним топиком, поэтому рекомендуется сделать id топика первой компонентой ключа. В качестве id топика можно выдавать последовательные целые числа или формировать их на основе текущего времени. Однако это будет означать, что при создании новых топиков им будут выдаваться очень близкие номера, а поскольку новые комментарии чаще появляются в новых топиках, вся нагрузка на запись будет идти в один таблет.
Поэтому, чтобы обеспечить равномерную загрузку таблетов при масштабировании таблицы, в качестве id топика используется случайно сформированные guid. В качестве id комментариев в топике используются их номера в порядке создания — это может приводить к появлению близких ключей, но если размеры топиков будут не слишком велики, шардирования по guid топиков должно оказаться достаточно.

Также в ключевой колонке `parent_path` хранится путь к комментарию в топике: для корневого комментария — его id, а `parent_path` каждого следующего комментария получается из `parent_path` его родителя путём дописывания к нему `/` и id родителя. Например, `parent_path` комментария с идентификатором `#id1`, написанного в ответ на корневой комментарий с идентификатором `0`, записывается как `0/#id1`. Такая организация пути позволяет быстро отфильтровать комментарии в поддереве при вызове метода `topic_comments`: если `parent_path` корневого комментария в поддереве — строка `S`, то `parent_path` любого его потомка будет иметь вид `S/#id1/.../#idN`. Он будет содержать `S` в качестве префикса. Поэтому, если лексикографически упорядочить комментарии в топике по `parent_path`, любому поддереву будет соответствовать непрерывный блок комментариев, начинающийся с корневого комментария поддерева. Таким образом, назначение `parent_path` ключевой колонкой позволяет не выполнять full scan таблицы для выбора комментариев в поддереве.

Дополнительно для каждого комментария хранится id непосредственного комментария-родителя. Это нужно, чтобы при отображении комментариев их можно было собрать в дерево. Кроме того, в таблице `topic_comments` хранится счетчик просмотров комментария, для которого используется механизм агрегирующих колонок. Такой механизм позволяет выполнять инкремент и декремент значения колонки без чтения ее предыдущей версии.

#### Таблица `topic_comments`

| Имя колонки | Тип | Ключевая колонка | Описание |
| --- | --- | --- | --- |
| `topic_id` | `guid` | Да | Guid топика. |
| `parent_path` | `string` | Да | Путь к комментарию. |
| `comment_id` | `uint64` | Нет | Id комментария. Его порядковый номер при добавлении в топик. |
| `parent_id` | `uint64` | Нет | Id комментария, в ответ на который был написан данный комментарий. Для корневых комментариев совпадает с `comment_id`. |
| `user` | `string` | Нет | Логин пользователя, оставившего комментарий. |
| `create_time` | `uint64` | Нет | Время создания комментария в POSIX Time. |
| `update_time` | `uint64` | Нет | Время последнего обновления комментария в POSIX Time. |
| `content` | `string` | Нет | Текст комментария. |
| `views_count` | `int64` | Нет | Количество просмотров, агрегирующая колонка. |
| `deleted` | `boolean` | Нет | Флаг того, что комментарий был удалён. |

Чтобы выбирать записи в вызове `/last_user_comments`, таблица `topic_comments` не подходит. Комментарии пользователя могут находиться в разных топиках, а значит, для выполнения подобного запроса потребуется full scan таблицы. {{product-name}} не поддерживает вторичные индексы, поэтому их также не получится использовать.

Поэтому необходима вторая, вспомогательная таблица  `user_comments`, где в качестве первой компоненты первичного ключа используется имя пользователя. Из соображений равномерного распределения нагрузки при будущем шардировании таблицы, стоит добавить в начало ключа вычислимую колонку — хеш от `user_name`. Для хеширования используется функция farm_hash, указываемая в поле `expression` в схеме таблицы (смотрите код создания таблиц).

#### Таблица `user_comments`

| Имя колонки | Тип |  Ключевая колонка |  Описание |
| --- | --- | --- | --- |
| `hash(user_name)` | `uint64` | Да | Вычислимая колонка. |
| `user_name` | `string` | Да | Логин пользователя, оставившего комментарий. |
| `topic_id` | `string` | Да | Guid топика. |
| `parent_path` | `string` | Да | Путь к комментарию. |
| `update_time` | `uint64` | Нет | Время последнего обновления комментария в POSIX Time. |

Необходимо решить, как эффективно обрабатывать вызов `/last_topics`. Простой способ — сделать запросы к таблице `topic_comments` с группировкой по `topic_id`. Но такой подход потребует повторно выполнить full scan самой большой таблицы. Поэтому имеет смысл завести еще одну небольшую таблицу `topics`, где по `topic_id` будет храниться время последнего обновления топика — то есть любого его комментария.

Несмотря на то, что данная таблица будет просматриваться целиком, ее размер гораздо меньше. Это позволит применить ряд оптимизаций. Например, загрузить таблицу целиком в память или увеличить параллельность выполнения запроса с помощью увеличения количества таблетов. Более того, ответ на такой запрос можно кэшировать, поскольку он будет одинаковым для всех пользователей. Также в данной таблице будет храниться количество комментариев в топике, чтобы при добавлении нового комментария можно было получить его идентификатор.

#### Таблица `topic`

| Имя колонки | Тип | Ключевая колонка |   Описание |
| --- | --- | --- | --- |
| `topic_id` | `string` | Да | Guid топика. |
| `update_time` | `uint64` | Нет | Время последнего обновления комментария в топике. |
| `comment_count` | `uint64` | Нет | Количество комментариев в топике, включая удаленные. |

##  Создание и монтирование таблиц { #tables_preparation }

В данном примере использована синхронная репликация с одной синхронной и одной асинхронной репликой и автоматическим переключением режима реплик. Данный режим обеспечивает самые строгие гарантии консистентности — как у нереплицированных динамических таблиц и не требует ручного переключения режима реплик.

Создание реплицированной таблицы состоит из следующих шагов:

1. Создание специального объекта типа `replicated_table` на мета-кластере.
2. Создание таблицы-реплики на других кластерах и соответствующие им объекты типа `table_replica` на мета-кластере.

Все таблицы необходимо создавать с одинаковой схемой. Для хранения данных используется медиум `ssd_blobs`. Выбор медиума SSD обусловлен необходимыми таймингами и потоком записи в мета-кластер. Сначала все реплики будут созданы в режиме `async`. После монтирования таблиц синхронная реплика будет выбрана автоматически.

Описанные выше действия можно выполнять как через CLI — через команды `yt create` и `yt set`, так и через Python wrapper.

Для данного примера написан скрипт создания необходимых таблиц, который считывает необходимые параметры в аргументах командной строки. Это позволяет использовать скрипт как в production, так и в testing окружении. Параметры скрипта:

* `meta_cluster` — кластер с реплицированной таблицей.
* `replica_clusters` — кластеры с репликами.
* `path` — путь к рабочей директории проекта.
* `force` — флаг, используемый для того, чтобы пересоздать таблицы, если они уже существуют. Без него изменения не будут внесены.

Пример запуска скрипта после сборки через `ya make`:

```bash
./create_tables --path //path/to/directory/ --meta_cluster <meta_cluster_name> --replica_clusters <replica1-cluster-name> <replica2-cluster-name> --force
```
## Квоты на сервис

Создайте выделенный tablet_cell_bundle и аккаунт, в котором будет квота на SSD. Для обеспечения отказоустойчивости сервиса, стоит иметь 3 кластера: мета-кластер и 2 кластера-реплики. Но в учебных целях можно все операции можно сделать на одном кластере. Он будет выступать мета-кластером и двумя репликами (синхронной и асинхронной).

##  Разработка кода сервиса { #development }

### Установка необходимых пакетов: [Flask](http://flask.pocoo.org/) и [WTForms](https://wtforms.readthedocs.io). WTForms будет использоваться для валидации параметров. Команды установки:

  ```bash
  sudo pip install flask
  sudo pip install wtforms
  ```

### Определение необходимых функций. Для передачи параметров программе будут использоваться переменные окружения. В дальнейшем такой подход позволит легко переносить приложение из testing окружения в production.

Необходимые переменные окружения:

* `CLUSTER` — имя кластера, на котором хранятся реплицированные таблицы.
* `TABLE_PATH` — путь к директории с таблицами.

Установка необходимых переменных окружения:

```bash
export CLUSTER=<cluster-name>
export TABLE_PATH=//path/to/directory
```
Создайте функцию для добавления информации о комментариях во все таблицы. Ее также можно использовать при редактировании комментариев и при добавлении комментария для примера.

### Реализация вызова `find_comment`

Функция для поиска комментариев в таблице принимает значения `topic_id` и `parent_path` в качестве аргументов. Это позволяет быстро находить комментарий с помощью метода `lookup_rows`, поскольку известны значения всех ключевых колонок таблицы `topic_comments`.

Пример получения информации про последние добавленные комментарии:

  ```python
  # -*- coding: utf-8 -*-
  import yt.wrapper as yt
  import json
  import os


  def find_comment(topic_id, parent_path, client, table_path):
      # В lookup_rows требуется передать значения всех ключевых колонок
      return list(client.lookup_rows(
          "{}/topic_comments".format(table_path),
          [{"topic_id": topic_id, "parent_path": parent_path}],
      ))


  def main():
      table_path = os.environ["TABLE_PATH"]
      <cluster-name> = os.environ["CLUSTER"]
      client = yt.YtClient(cluster_name, config={"backend": "rpc"})

      comment_info = find_comment(
          topic_id="1dd64501-4131025-562332a3-40507acc",
          parent_path="0",
          client=client, table_path=table_path,
      )
      print(json.dumps(comment_info, indent=4))


  if __name__ == "__main__":
      main()

  """
  Вывод программы:

  [
      {
          "update_time": 100000,
          "views_count": 0,
          "parent_id": 0,
          "deleted": false,
          "comment_id": 0,
          "creation_time": 100000,
          "content": "Some comment text",
          "parent_path": "0",
          "user": "abc",
          "topic_id": "1dd64501-4131025-562332a3-40507acc"
      }
  ]
  """
  ```

### Реализация вызова `post_comment`

Первая реализация вызова `post_comment` представляет собой функцию, принимающую все параметры в аргументах и возвращающую JSON-строку в качестве ответа. Запросы к динамическим таблицам собраны в одну транзакцию. Добавлена обработка исключений при обращении к системе {{product-name}}.

  ```python
  # -*- coding: utf-8 -*-
  import yt.wrapper as yt
  import os
  import json
  import time
  from datetime import datetime


  # Вспомогательная функция для получения количества комментариев в топике из таблицы topic_id
  # В случае отсутствия заданного топика возвращается None
  def get_topic_size(client, table_path, topic_id):
      topic_info = list(client.lookup_rows(
          "{}/topics".format(table_path), [{"topic_id": topic_id}],
      ))
      if not topic_info:
          return None
      assert(len(topic_info) == 1)
      return topic_info[0]["comment_count"]


  def post_comment(client, table_path, user, content, topic_id=None, parent_id=None):
      # Необходимо обрабатывать исключение YtResponseError, возникающее если выполнить операцию не удается
      try:
          # Добавление комментария включает в себя несколько запросов разных типов,
          # поэтому требуется собрать их в одну транзакицию, чтобы обеспечить атомарность
          with client.Transaction(type="tablet"):
              new_topic = not topic_id
              if new_topic:
                  topic_id = yt.common.generate_uuid()
                  comment_id = 0
                  parent_id = 0
                  parent_path = "0"
              else:
                  # Поле comment_id задается равным порядковому номеру комментария в топике
                  # Этот номер совпадает с текущим размером топика
                  comment_id = get_topic_size(topic_id)
                  if not comment_id:
                      return json.dumps({"error": "There is no topic with id {}".format(topic_id)})

                  parent_info = find_comment(topic_id, parent_path, client, table_path)
                  if not parent_info:
                      return json.dumps({"error" : "There is no comment {} in topic {}".format(parent_id, topic_id)})
                  parent_id = parent_info[0]["comment_id"]
                  parent_path = "{}/{}".format(parent_path, comment_id)

              creation_time = int(time.mktime(datetime.now().timetuple()))
              insert_comments([{
                  "topic_id": topic_id,
                  "comment_id": comment_id,
                  "parent_id": parent_id,
                  "parent_path": parent_path,
                  "user": user,
                  "creation_time": creation_time,
                  "update_time": creation_time,
                  "content": content,
                  "views_count": 0,
                  "deleted": False,
              }], client, table_path)

              result = {"comment_id" : comment_id, "new_topic" : new_topic, "parent_path": parent_path}
              if new_topic:
                  result["topic_id"] = topic_id
              return json.dumps(result)
      except yt.YtResponseError as error:
          # У yt.YtResponseError определен метод __str__, возвращающий подробное сообщение об ошибке
          json.dumps({"error" : str(error)})
  ```

### Реализация вызова `user_comments`

Функция включает в себя только один запрос `select_rows`. В качестве основной таблицы используется `user_comments`, поскольку она отсортирована по полю `user`, и из неё эффективнее всего извлекать комментарии заданного пользователя. К данной таблице с помощью `JOIN` подключается дополнительная таблица `topic_comments`, чтобы получить полную информацию о комментариях. При этом в секции `ON` необходимо явно задать и `topic_id`, и `parent_path`, чтобы данные из дополнительной таблицы можно было эффективно получить по ключу. В секциях `WHERE` и `LIMIT` задаются необходимые фильтры, в секции `ORDER BY` — порядок, чтобы сперва возвращались самые новые комментарии.

  ```python
  # -*- coding: utf-8 -*-
  import yt.wrapper as yt
  import os
  import json


  def get_last_user_comments(client, table_path, user, limit=10, from_time=0):
      try:
          # В качестве основной таблицы используется user_comments, позволяющая фильтровать записи по полю user
          # Через join подключается дополнительная таблица topic_comments,
          # которая используется для получения полной информации про комментарий
          comments_info = list(client.select_rows(
              """
              topic_comments.topic_id as topic_id,
              topic_comments.comment_id as comment_id,
              topic_comments.content as content,
              topic_comments.user as user,
              topic_comments.views_count as views_count,
              topic_comments.update_time as update_time
              from [{0}/user_comments] as user_comments join [{0}/topic_comments] as topic_comments
              on (user_comments.topic_id, user_comments.parent_path) =
              (topic_comments.topic_id, topic_comments.parent_path)
              where user_comments.user = '{1}' and user_comments.update_time >= {2}
              order by user_comments.update_time desc
              limit {3}""".format(table_path, user, from_time, limit)
          ))
          return json.dumps(comments_info, indent=4)
      except yt.YtResponseError as error:
          return json.dumps({"error" : str(error)})
  ```

Запуск приложения. Приложение создаётся через [Blueprint application factory](http://flask.pocoo.org/docs/1.0/blueprints/). Для передачи параметров `client` и `table_path` используется специальный объект `Flask.g`, в котором настраивается контекст приложения. Передача настроек для подключения выполняется через переменные окружения `HOST` и `PORT`. По умолчанию используется http://127.0.0.1:5000/. Так же настраивается логирование: в файл `comment_service.log` производится запись одного сообщения в лог в начале выполнения запроса и еще одного при его завершении. В файл `driver.log` перенаправляются отладочные логи системы {{product-name}}.

Для каждого запроса требуется создавать отдельный клиент {{product-name}}, чтобы в дальнейшем было возможно обрабатывать запросы параллельно. При этом для каждого клиента создается собственная версия драйвера, что приводит к дополнительному расходу ресурсов. Чтобы избежать данной проблемы, создается один общий объект драйвера при инициализации приложения, после чего данный объект подключается к каждому создаваемому клиенту путем выставления в клиенте соответствующей опции.

<!--[//]: TODO: Код инициализации и запуска приложения доступен по [ссылке](yt/examples/comment_service/tutorial/fragments/app.py).-->

Для валидации параметров запроса используются формы WTForms. Для каждого метода требуется создать отдельный класс-форму, в которой указываются параметры запроса, их тип и т.п.
<!--[//]: TODO: Новая версия метода `post_comment` выглядит [следующим образом](/yt/examples/comment_service/tutorial/fragments/post_comment.py).
[//]: TODO: Код метода `user_comments` доступен по [ссылке](/yt/examples/comment_service/tutorial/fragments/user_comments.py).-->

Предполагается что код сохранён в файле `run_application.py`.

```bash
export CLUSTER=<cluster-name>
export TABLE_PATH=//path/to/directory
export HOST=127.0.0.1
export PORT=5000
python run_application.py
```


