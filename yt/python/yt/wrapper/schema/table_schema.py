from .helpers import check_schema_module_available, is_schema_module_available
from .types import is_yt_dataclass
from .internal_schema import _py_schema_to_ti_type, _create_py_schema

from ..errors import YtError

import yt.yson

import copy

try:
    import yandex.type_info.typing as ti
except ImportError:
    pass


def _check_ti_available():
    check_schema_module_available(skiff=False, py3=False)


class ColumnSchema(object):
    """ Class representing table column schema.

    See https://yt.yandex-team.ru/docs/description/storage/static_schema.html#schema_overview
    """

    def __init__(self, name, type_, sort_order=None, group=None):
        """type_ may be either a type_info type or a @yt.wrapper.schema.dataclass"""
        _check_ti_available()
        self.name = name

        if ti.is_valid_type(type_):
            self.type = type_
        elif is_schema_module_available() and is_yt_dataclass(type_):
            self.type = _py_schema_to_ti_type(_create_py_schema(type_))
        else:
            raise TypeError("Expected type_info type or class marked with @yt.wrapper.schema.yt_dataclass, "
                            "got <{}>{!r}".format(type(type_), type_))

        self.sort_order = sort_order
        self.group = group

    def to_yson_type(self):
        result = {
            "name": self.name,
            "type_v3": yt.yson.loads(ti.serialize_yson(self.type)),
        }
        if self.sort_order is not None:
            result["sort_order"] = self.sort_order
        if self.group is not None:
            result["group"] = self.group
        return result

    @classmethod
    def from_yson_type(cls, obj):
        _check_ti_available()
        type = ti.deserialize_yson(yt.yson.dumps(obj["type_v3"]))
        return ColumnSchema(obj["name"], type, sort_order=obj.get("sort_order"), group=obj.get("group"))

    def __eq__(self, other):
        if not isinstance(other, ColumnSchema):
            return False
        return (self.name, self.type, self.sort_order, self.group) == \
            (other.name, other.type, other.sort_order, other.group)

    def __ne__(self, other):
        return not (self == other)

    def __str__(self):
        return str(self.to_yson_type())

    def __getstate__(self):
        return self.to_yson_type()

    def __setstate__(self, d):
        self.__dict__ = ColumnSchema.from_yson_type(d).__dict__


class TableSchema(object):
    """Class representing table schema.

    It can be built using the constructor or fluently using add_column method:

        TableSchema() \
            .add_column("key", ti.String, sort_order="ascending") \
            .add_column("value", ti.List[ti.Int32])

    See https://yt.yandex-team.ru/docs/description/storage/static_schema.html#schema_overview
    """

    def __init__(self, columns=None, strict=True, unique_keys=False):
        _check_ti_available()
        if columns is None:
            self.columns = []
        else:
            self.columns = columns[:]
        self.strict = strict
        self.unique_keys = unique_keys

    @classmethod
    def from_row_type(cls, row_type, strict=True, unique_keys=False):
        check_schema_module_available()
        if not is_yt_dataclass(row_type):
            raise TypeError("Expected class marked with @yt.wrapper.schema.yt_dataclass, got {}"
                            .format(row_type.__qualname__))
        py_schema = _create_py_schema(row_type)
        if strict and py_schema._other_columns_field is not None:
            raise YtError('Cannot infer strict schema from yt_dataclass "{}" with field marked with "OtherColumns"'
                          .format(row_type.__qualname__))
        columns = [
            ColumnSchema(column._yt_name, _py_schema_to_ti_type(column._py_schema))
            for column in py_schema._fields
        ]
        return cls(columns, strict=strict, unique_keys=unique_keys)

    def add_column(self, *args, **kwargs):
        """Add column.

        Call as either .add_column(ColumnSchema(...)) or .add_column(name, type, ...).
        """
        if len(args) == 1:
            if not isinstance(args[0], ColumnSchema):
                raise TypeError("If add_column() is called with single argument, it must be of type ColumnSchema")
            self.columns.append(args[0])
        else:
            self.columns.append(ColumnSchema(*args, **kwargs))
        return self

    def build_schema_sorted_by(self, column_names, sort_order="ascending"):
        column_name_to_column = {
            column.name: copy.deepcopy(column)
            for column in self.columns
        }
        sorted_columns = []
        for column_name in column_names:
            column = column_name_to_column.get(column_name)
            if column is None:
                raise ValueError("Column \"{}\" is not found".format(column_name))
            column.sort_order = sort_order
            sorted_columns.append(column)
            del column_name_to_column[column_name]
        for column in column_name_to_column.values():
            sorted_columns.append(column)
        return TableSchema(columns=sorted_columns, strict=self.strict, unique_keys=self.unique_keys)

    def to_yson_type(self):
        columns = yt.yson.to_yson_type([c.to_yson_type() for c in self.columns])
        columns.attributes["strict"] = self.strict
        columns.attributes["unique_keys"] = self.unique_keys
        return columns

    @classmethod
    def from_yson_type(cls, obj):
        _check_ti_available()
        columns = [ColumnSchema.from_yson_type(c) for c in obj]
        attrs = obj.attributes
        kwargs = {}
        if "strict" in attrs:
            kwargs["strict"] = attrs["strict"]
        if "unique_keys" in attrs:
            kwargs["unique_keys"] = attrs["unique_keys"]
        return TableSchema(columns, **kwargs)

    def is_empty_nonstrict(self):
        return not self.strict and len(self.columns) == 0

    def __eq__(self, other):
        if not isinstance(other, TableSchema):
            return False
        return (self.columns, self.strict, self.unique_keys) == \
            (other.columns, other.strict, other.unique_keys)

    def __ne__(self, other):
        return not (self == other)

    def __repr__(self):
        return "TableSchema({})".format(self.to_yson_type())    