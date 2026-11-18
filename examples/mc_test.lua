-- http mod: microcontroller test (Lua block, video out -> monitor)
-- Every 5 s: GET https://httpbin.org/uuid, shows the status and the uuid.
-- Outputs: number 1 = responses received, bool 1 = last request succeeded.
-- Microcontrollers cannot reach localhost/LAN by default (http.cfg: local_network).
U="https://httpbin.org/uuid"
id=nil st="?" last="" n=0 ok=false t=0
function onTick()
 if not http then st="no http" return end
 t=t+1
 if not id and t%300==1 then
  id,err=http.get(U,"timeout=10;headers=0")
  st=id and "waiting" or ("refused "..tostring(err))
 end
 if id then
  local i,k,a,b=http.poll(id)
  if i then
   id=nil
   if k=="response" then n=n+1 ok=a==200 st="HTTP "..a last=http.field(b,"uuid") or b
   else ok=false st="err "..tostring(a) end
  end
 end
 output.setNumber(1,n)
 output.setBool(1,ok)
end
function onDraw()
 screen.setColor(0,255,0)
 screen.drawText(2,2,st)
 screen.drawText(2,10,"rx "..n)
 screen.drawText(2,18,tostring(last):sub(1,24))
end
