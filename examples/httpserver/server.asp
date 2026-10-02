<%
Option Explicit

Const MaxHeaderBytes = 16384
Const ReadChunkBytes = 4096

Function ReadRequestHeaders(client)
    Dim data
    Dim chunk
    Dim headerEnd

    data = ""
    headerEnd = 0

    Do While LenB(data) <= MaxHeaderBytes
        chunk = client.Receive(ReadChunkBytes)
        If LenB(chunk) = 0 Then
            Exit Do
        End If

        data = data & chunk
        headerEnd = InStr(1, data, vbCrLf & vbCrLf)
        If headerEnd > 0 Then
            Exit Do
        End If
    Loop

    ReadRequestHeaders = data
End Function

Sub SendAll(client, data)
    Dim position
    Dim remaining
    Dim sent

    position = 1
    Do While position <= LenB(data)
        remaining = LenB(data) - position + 1
        sent = client.Send(MidB(data, position, remaining))
        If sent <= 0 Then
            Exit Do
        End If
        position = position + sent
    Loop
End Sub

Function ResponseHead(statusText, contentType, contentLength)
    ResponseHead = "HTTP/1.1 " & statusText & vbCrLf & _
        "Server: OpenASP-Example" & vbCrLf & _
        "Content-Type: " & contentType & vbCrLf & _
        "Content-Length: " & CStr(contentLength) & vbCrLf & _
        "Connection: close" & vbCrLf & _
        vbCrLf
End Function

Sub HandleClient(client)
    Dim requestData
    Dim lineEnd
    Dim requestLine
    Dim requestParts
    Dim method
    Dim target
    Dim queryAt
    Dim statusText
    Dim contentType
    Dim body
    Dim responseData

    requestData = ReadRequestHeaders(client)
    If LenB(requestData) = 0 Then
        Exit Sub
    End If

    If LenB(requestData) > MaxHeaderBytes Then
        statusText = "431 Request Header Fields Too Large"
        contentType = "text/plain; charset=utf-8"
        body = "request headers are too large" & vbLf
    Else
        lineEnd = InStr(1, requestData, vbCrLf)
        If lineEnd <= 1 Then
            statusText = "400 Bad Request"
            contentType = "text/plain; charset=utf-8"
            body = "bad request" & vbLf
        Else
            requestLine = LeftB(requestData, lineEnd - 1)
            requestParts = Split(requestLine, " ")

            If UBound(requestParts) < 2 Then
                statusText = "400 Bad Request"
                contentType = "text/plain; charset=utf-8"
                body = "bad request" & vbLf
            Else
                method = UCase(requestParts(0))
                target = requestParts(1)
                queryAt = InStr(1, target, "?")
                If queryAt > 0 Then
                    target = Left(target, queryAt - 1)
                End If

                If method <> "GET" And method <> "HEAD" Then
                    statusText = "405 Method Not Allowed"
                    contentType = "text/plain; charset=utf-8"
                    body = "method not allowed" & vbLf
                ElseIf target = "/health" Then
                    statusText = "200 OK"
                    contentType = "text/plain; charset=utf-8"
                    body = "ok" & vbLf
                ElseIf target = "/" Then
                    statusText = "200 OK"
                    contentType = "text/html; charset=utf-8"
                    body = "<!doctype html><html><head><meta charset=""utf-8""><title>OpenASP HTTP Server</title></head><body><h1>OpenASP HTTP Server</h1><p>Served entirely by ASP code.</p></body></html>"
                Else
                    statusText = "404 Not Found"
                    contentType = "text/plain; charset=utf-8"
                    body = "not found" & vbLf
                End If
            End If
        End If
    End If

    responseData = ResponseHead(statusText, contentType, LenB(body))
    If method <> "HEAD" Then
        responseData = responseData & body
    End If
    Call SendAll(client, responseData)
End Sub

Dim host
Dim port
Dim portText
Dim maxRequests
Dim maxRequestsText
Dim served
Dim listener
Dim client

host = Request("host")
If Len(host) = 0 Then
    host = "127.0.0.1"
End If

portText = Request("port")
If Len(portText) = 0 Then
    port = 18080
Else
    port = CInt(portText)
End If

maxRequestsText = Request("maxRequests")
If Len(maxRequestsText) = 0 Then
    maxRequests = 0
Else
    maxRequests = CInt(maxRequestsText)
End If
served = 0

Set listener = Server.CreateObject("OpenASP.Socket")
listener.Timeout = 0
Call listener.Listen(host, port, 128)

Do
    Set client = listener.Accept()
    If IsObject(client) Then
        Call HandleClient(client)
        Call client.Close()
        served = served + 1
    End If

    If maxRequests > 0 And served >= maxRequests Then
        Exit Do
    End If
Loop

Call listener.Close()
Response.Write "OpenASP HTTP server stopped after " & CStr(served) & " request(s)." & vbLf
%>
