<%
Set request = Server.CreateObject("MSXML2.ServerXMLHTTP.6.0")
Call request.open("GET", Request("url"), False)
Call request.setRequestHeader("X-Component-Test", "yes")
Call request.send()
Response.Write request.readyState
Response.Write ":"
Response.Write request.status
Response.Write ":"
Response.Write request.responseText
Response.Write ":"
Response.Write request.getResponseHeader("content-type")
%>
