<%
Option Explicit

Const MaxHeaderBytes = 16384
Const ReadChunkBytes = 4096
Const MaximumWorkers = 64

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

Function ResponseHead(statusText, contentType, contentLength, workerSlot)
    ResponseHead = "HTTP/1.1 " & statusText & vbCrLf & _
        "Server: OpenASP-Multiprocess-Example" & vbCrLf & _
        "X-OpenASP-Worker: " & CStr(workerSlot) & vbCrLf & _
        "Content-Type: " & contentType & vbCrLf & _
        "Content-Length: " & CStr(contentLength) & vbCrLf & _
        "Connection: close" & vbCrLf & _
        vbCrLf
End Function

Sub HandleClient(client, workerSlot)
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
                    body = "ok from worker " & CStr(workerSlot) & vbLf
                ElseIf target = "/" Then
                    statusText = "200 OK"
                    contentType = "text/html; charset=utf-8"
                    body = "<!doctype html><html><head><meta charset=""utf-8""><title>OpenASP Multiprocess HTTP Server</title></head><body><h1>OpenASP Multiprocess HTTP Server</h1><p>Worker " & CStr(workerSlot) & " served this request.</p></body></html>"
                Else
                    statusText = "404 Not Found"
                    contentType = "text/plain; charset=utf-8"
                    body = "not found" & vbLf
                End If
            End If
        End If
    End If

    responseData = ResponseHead(statusText, contentType, LenB(body), workerSlot)
    If method <> "HEAD" Then
        responseData = responseData & body
    End If
    Call SendAll(client, responseData)
End Sub

Function RunWorker(listener, workerSlot, maxRequestsPerWorker)
    Dim served
    Dim client

    served = 0
    Do
        Set client = listener.Accept()
        If IsObject(client) Then
            Call HandleClient(client, workerSlot)
            Call client.Close()
            served = served + 1
        End If

        If maxRequestsPerWorker > 0 And served >= maxRequestsPerWorker Then
            Exit Do
        End If
    Loop

    RunWorker = served
End Function

Dim host
Dim port
Dim portText
Dim workerCount
Dim workerCountText
Dim maxRequestsPerWorker
Dim maxRequestsText
Dim listener
Dim process
Dim workerSlot
Dim childPid
Dim reapedPid
Dim remainingWorkers

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

workerCountText = Request("workers")
If Len(workerCountText) = 0 Then
    workerCount = 4
Else
    workerCount = CInt(workerCountText)
End If
If workerCount < 1 Then
    workerCount = 1
ElseIf workerCount > MaximumWorkers Then
    workerCount = MaximumWorkers
End If

maxRequestsText = Request("maxRequestsPerWorker")
If Len(maxRequestsText) = 0 Then
    maxRequestsPerWorker = 0
Else
    maxRequestsPerWorker = CInt(maxRequestsText)
End If
If maxRequestsPerWorker < 0 Then
    maxRequestsPerWorker = 0
End If

' The listener must exist before Fork so every worker inherits the same fd.
Set listener = Server.CreateObject("OpenASP.Socket")
listener.Timeout = 0
Call listener.Listen(host, port, 128)

Set process = Server.CreateObject("OpenASP.Process")
For workerSlot = 1 To workerCount
    childPid = process.Fork()
    If childPid = 0 Then
        Call RunWorker(listener, workerSlot, maxRequestsPerWorker)
        Call listener.Close()
        Call process.Exit(0)
    End If
Next

' The parent supervises only; workers retain their inherited listener copies.
Call listener.Close()
remainingWorkers = workerCount
Do While remainingWorkers > 0
    reapedPid = process.WaitPid(-1, -1)
    If reapedPid > 0 Then
        remainingWorkers = remainingWorkers - 1
    End If
Loop

Response.Write "OpenASP multiprocess HTTP server stopped after all " & _
    CStr(workerCount) & " worker(s) exited." & vbLf
%>
