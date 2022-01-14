# -*- coding: utf-8 -*-

import yt.wrapper
import yt.wrapper.schema as schema

import getpass


@yt.wrapper.yt_dataclass
class ValueRow:
    # Можно указывать точный тип, который соответствует типу в таблице.
    # Обычный int в данном случае -- то же самое.
    value: schema.Int64


@yt.wrapper.yt_dataclass
class SumRow:
    sum: schema.Int64


@yt.wrapper.yt_dataclass
class RowIndexRow:
    row_index: schema.Int64


@yt.wrapper.aggregator
class Mapper(yt.wrapper.TypedJob):
    # Типы входных и выходных строк можно задавать сразу для нескольких таблиц.
    def prepare_operation(self, context, preparer):
        preparer.inputs([0, 1, 2], type=ValueRow).outputs([0, 1], type=SumRow)

    def __call__(self, rows):
        sum = 0
        for row, context in rows.with_context():
            # Номер входной таблицы хранится в context-е.
            input_table_index = context.get_table_index()
            if input_table_index == 0:
                sum += row.value
            else:
                sum -= row.value
            output_row = SumRow(sum=sum)
            output_table_index = sum % 2

            # Для указания номера выходной таблицы нужно использовать
            # класс OutputRow.
            yield yt.wrapper.OutputRow(output_row, table_index=output_table_index)


class Reducer(yt.wrapper.TypedJob):
    def prepare_operation(self, context, preparer):
        preparer.input(0, type=ValueRow).output(0, type=RowIndexRow)

    # Пример получения row_index в reducer с помощью context.
    def __call__(self, rows):
        for row, context in rows.with_context():
            yield RowIndexRow(row_index=context.get_row_index())


if __name__ == "__main__":
    client = yt.wrapper.YtClient(proxy="freud")

    path = "//tmp/{}-table-switches".format(getpass.getuser())
    client.create("map_node", path, ignore_existing=True)

    input1, input2, input3 = inputs = ["{}/input{}".format(path, i) for i in range(1, 4)]
    client.write_table_structured(input1, ValueRow, [ValueRow(value=7)])
    client.write_table_structured(input2, ValueRow, [ValueRow(value=3)])
    client.write_table_structured(input3, ValueRow, [ValueRow(value=4)])

    output1, output2 = outputs = ["{}/output{}".format(path, i) for i in range(1, 3)]

    # Пример запуска маппера, который будет использовать OutputRow
    # для выбора выходной таблицы.
    client.run_map(
        Mapper(),
        inputs,
        outputs,
    )
    # В первую таблицу попадают чётные суммы.
    assert list(client.read_table_structured(output1, SumRow)) == [SumRow(sum=4), SumRow(sum=0)]
    # Во вторую таблицу попадают нечётные суммы.
    assert list(client.read_table_structured(output2, SumRow)) == [SumRow(sum=7)]

    client.remove(input1)
    client.write_table_structured(
        "<sorted_by=[value]>" + input1,
        ValueRow,
        [ValueRow(value=1), ValueRow(value=3), ValueRow(value=4)],
    )

    # Пример запуска редьюсера, который получает индексы строк из контекста.
    client.remove(output1)
    client.run_reduce(
        Reducer(),
        input1,
        output1,
        reduce_by=["value"],
    )
    assert list(client.read_table_structured(output1, RowIndexRow)) == [
        RowIndexRow(row_index=0),
        RowIndexRow(row_index=1),
        RowIndexRow(row_index=2),
    ]