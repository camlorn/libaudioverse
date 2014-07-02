{%-import 'macros.t' as macros with context-%}
#implements lifting the raw ctypes-basedd api into something markedly pallatable.
#among other things, the implementation heree enables calling functions with keyword arguments and raises exceptions on error, rather than dealing with ctypes directly.
import ctypes
import _libaudioverse

#These are not from libaudioverse.
#Implement a method by which the public libaudioverse module may register its exception classes for error code translation.
class PythonBindingsCouldNotTranslateErrorCodeError(object):
	"""An exception representing failure to translate a libaudioverse error code into a python exception.  If you see this, report it as a bug with Libaudioverse because something has gone very badly wrong."""
	pass

errors_to_exceptions = dict()

def bindings_register_exception(code, cls):
	errors_to_exceptions[code] = cls

def make_error_from_code(err):
	"""Internal use.  Translates libaudioverse error codes into exceptions."""
	return errors_to_exceptions.get(err, PythonBindingsCouldNotTranslateErrorCodeError)()

{%for func_name, friendly_name in friendly_functions.iteritems()%}
{%-set func_info = functions[func_name]-%}
{%-set input_arg_names = func_info.input_args|map(attribute='name')|list-%}
{%-set output_arg_names = func_info.output_args|map(attribute='name')|list-%}
{%-if func_info.output_args|length == 0-%}
def {{friendly_name}}({{input_arg_names|join(', ')}}):
	err = _libaudioverse.{{func_name}}({{input_arg_names|join(', ')}})
	if err != _libaudioverse.Lav_ERROR_NONE:
		raise make_error_from_code(err)
{%else-%}
def {{friendly_name}}({{input_arg_names|join(', ')}}):
	{%-for i in func_info.output_args%}
	{{i.name}} = {{macros.destination_string(i.type)}}()
{%-endfor%}
	err = _libaudioverse.{{func_name}}({{input_arg_names|join(', ')}}{%if input_arg_names|length > 0%}, {%endif%}
		{%for i in output_arg_names%}ctypes.byref({{i}}){%if not loop.last%}, {%endif%}{%endfor%})
	if err != _libaudioverse.Lav_ERROR_NONE:
		raise make_error_from_code(err)
	return {{output_arg_names|join('.value, ')}}.value{#we need to do this because we're seriously circumventing ctypes automatic conversions here#}
{%endif%}
{%endfor%}