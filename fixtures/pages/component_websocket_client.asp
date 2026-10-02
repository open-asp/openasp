<%
Dim ws
Dim message

Set ws = Server.CreateObject("OpenASP.WebSocket")
ws.Timeout = 5000
Call ws.Connect("ws://127.0.0.1:19143/echo", "openasp.test")
Response.Write CStr(ws.State)
Response.Write "|" & CStr(ws.SendText("hello"))
Response.Write "|" & CStr(ws.SendBinary(ChrB(0) & ChrB(255)))
message = ws.Receive()
Response.Write "|" & message
Response.Write "|" & ws.MessageType
Response.Write "|" & CStr(ws.Opcode)
Response.Write "|" & CStr(ws.Final)
Call ws.Close()
Response.Write "|" & CStr(ws.State)
%>
