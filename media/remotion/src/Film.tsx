import {AbsoluteFill,Series,useCurrentFrame,useVideoConfig,staticFile,interpolate} from 'remotion';
import {Audio} from '@remotion/media';
import {Reveal} from './scenes/Reveal';
import {Conversation} from './scenes/Conversation';
import {Memory} from './scenes/Memory';
import {Proof} from './scenes/Proof';
import {Agents} from './scenes/Agents';
import {Research} from './scenes/Research';
import {Desktop} from './scenes/Desktop';
import {Connections} from './scenes/Connections';
import {Controls} from './scenes/Controls';
import {Architecture} from './scenes/Architecture';
import {Closing} from './scenes/Closing';
import captions from './captions.json';
import {C,font,clamp} from './design';

const Captions=()=>{const f=useCurrentFrame();const {fps}=useVideoConfig();const current=captions.find(c=>f/fps>=c.start&&f/fps<c.end);return current?<div style={{position:'absolute',left:200,right:200,bottom:21,textAlign:'center',fontSize:27,lineHeight:1.3,color:'#e5efff',fontFamily:font,textShadow:'0 2px 6px #000'}}>{current.text}</div>:null;};

const Transitions=()=>{
 const f=useCurrentFrame();const {fps}=useVideoConfig();
 const at=[12,25,49,63,82,102,120,136,154,170].find(t=>Math.abs(f-t*fps)<14);
 if(at===undefined)return null;
 const p=(f-at*fps+14)/28;
 const x=interpolate(p,[0,1],[-2800,2500],clamp);
 return <AbsoluteFill style={{pointerEvents:'none',overflow:'hidden'}}><div style={{position:'absolute',top:-400,bottom:-400,left:0,width:2300,transform:`translateX(${x}px) skewX(-18deg)`,background:'linear-gradient(90deg,#5ad2ff00,#132345 2%,#080d1a 9%,#080d1a 91%,#302954 98%,#a791ff00)',borderLeft:`2px solid ${C.cyan}66`,borderRight:`2px solid ${C.violet}66`,boxShadow:'0 0 65px #5ad2ff13'}}><div style={{position:'absolute',top:0,bottom:0,left:160,width:1,background:'#5ad2ff33'}}/><div style={{position:'absolute',top:0,bottom:0,right:160,width:1,background:'#a791ff33'}}/></div></AbsoluteFill>;
};

export const Film=()=> <AbsoluteFill>
 <Series>
  <Series.Sequence durationInFrames={360}><Reveal title={'Your next idea.\nAlready in context.'} subtitle="Meet the voice on your desktop, with a memory that stays."/></Series.Sequence>
  <Series.Sequence durationInFrames={390}><Conversation title="Keep your flow. Just talk." subtitle="Talk naturally. Interrupt freely. Keep your flow."/></Series.Sequence>
  <Series.Sequence durationInFrames={720}><Memory title="Plot twist: it remembers." subtitle="Your context can outlast the conversation."/></Series.Sequence>
  <Series.Sequence durationInFrames={420}><Proof title="Close the session. Keep the context." subtitle="A real persistence test of the integrated memory backend."/></Series.Sequence>
  <Series.Sequence durationInFrames={570}><Agents title="Keep talking. Keep things moving." subtitle="Fast answers up front. Background agents on the task."/></Series.Sequence>
  <Series.Sequence durationInFrames={600}><Research title="Meet Burnos’s research side." subtitle="A browser of its own. One less thing on your screen."/></Series.Sequence>
  <Series.Sequence durationInFrames={540}><Desktop title="Say it. Make it happen." subtitle="From a useful answer to useful work."/></Series.Sequence>
  <Series.Sequence durationInFrames={480}><Connections title="Your tools. Now part of the conversation." subtitle="Extend Burnos with the services you connect."/></Series.Sequence>
  <Series.Sequence durationInFrames={540}><Controls title="A little orb. A lot of control." subtitle="Personalize how Burnos listens, speaks, and works."/></Series.Sequence>
  <Series.Sequence durationInFrames={480}><Architecture title="Local foundations. Connected intelligence." subtitle="Built around the way you actually work."/></Series.Sequence>
  <Series.Sequence durationInFrames={270}><Closing title="burnos" subtitle="Your next idea already has a head start."/></Series.Sequence>
 </Series>
 <Audio src={staticFile('soundtrack-master.m4a')}/>
 <Transitions/>
 <Captions/>
 </AbsoluteFill>;
