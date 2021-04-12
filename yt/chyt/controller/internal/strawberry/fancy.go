package strawberry

import (
	"bytes"
	"text/template"

	"a.yandex-team.ru/yt/go/guid"
	"a.yandex-team.ru/yt/go/ypath"
	"a.yandex-team.ru/yt/go/yson"
	"a.yandex-team.ru/yt/go/yt"
)

func ToYsonURL(value interface{}) interface{} {
	return yson.ValueWithAttrs{
		Attrs: map[string]interface{}{"_type_tag": "url"},
		Value: value,
	}
}

func navigationURL(cluster string, path ypath.Path) interface{} {
	return ToYsonURL("https://yt.yandex-team.ru/" + cluster + "/navigation?path=" + path.String())
}

func operationURL(cluster string, opID yt.OperationID) interface{} {
	return ToYsonURL("https://yt.yandex-team.ru/" + cluster + "/operations/" + opID.String())
}

func opAnnotations(a *Agent, state *Oplet) map[string]interface{} {
	return map[string]interface{}{
		"strawberry_family": a.controller.Family(),
		"strawberry_node":   a.root.Child(state.Alias),
		"strawberry_controller": map[string]interface{}{
			"address": a.hostname,
			// TODO(max42): build revision, etc.
		},
		"strawberry_incarnation":        state.IncarnationIndex + 1,
		"strawberry_previous_operation": state.YTOpID,
	}
}

func opDescription(a *Agent, state *Oplet) map[string]interface{} {
	desc := map[string]interface{}{
		"strawberry_node":        navigationURL(a.Proxy, a.root.Child(state.Alias)),
		"strawberry_incarnation": state.IncarnationIndex + 1,
	}
	if state.YTOpID != yt.OperationID(guid.FromParts(0, 0, 0, 0)) {
		desc["strawberry_previous_operation_id"] = operationURL(a.Proxy, state.YTOpID)
	}
	return desc
}

func cypAnnotation(a *Agent, state *Oplet) string {
	data := struct {
		A     *Agent
		State *Oplet
	}{
		a,
		state,
	}

	t := template.Must(template.New("cypAnnotation").Parse(`
## Strawberry operation {{.State.Alias}}
Current operation id: [{{.State.YTOpID}}](https://yt.yandex-team.ru/{{.A.Proxy}}/operations/{{.State.YTOpID}})
Current incarnation: {{.State.IncarnationIndex}}
`))

	b := new(bytes.Buffer)
	if err := t.Execute(b, data); err != nil {
		panic(err)
	}

	return b.String()
}