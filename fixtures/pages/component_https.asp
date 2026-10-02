<%
On Error Resume Next

Dim http
Dim body

Set http = Server.CreateObject("OpenASP.HttpClient")
http.Timeout = 5000
http.TLSVerify = True
http.CAFile = Request("ca")
body = http.Get("https://localhost:19146/secure")
Response.Write CStr(http.Status) & "|" & body & "|" & CStr(http.TLSVerify) & "|" & CStr(Err.Number) & "|" & Err.Description
%>
