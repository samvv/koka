import std/os/path
import std/os/flags
import std/os/env
import std/os/file
import std/os/dir
import std/os/process

val header = "usage:\n stack exec koka -- util/docs [-- [options]]\n\noptions:"

struct iflags {
  publish  : bool  = False
  version  : string = ""
}

val flag-descs : list<flag<iflags>> = {
  fun set-publish( f : iflags, b : bool ) : iflags  { f(publish = b) }
  fun set-version( f : iflags, s : string ) : iflags  { f(version = s) }
  [ Flag( "p", ["publish"], Bool(set-publish), "publish to ../koka-pages directory" ),
    Flag( "v", ["version"], Req(set-version,"<version>"), "latest koka version" )
  ]
}

public fun process-flags() : <ndet,console> maybe<iflags> {
  val (flags,args,errs) = parse( Iflags(), flag-descs, get-args() )
  if (errs.is-nil && args.is-nil) {
    Just(flags)
  }
  else {
    println( errs.join("\n") + "\n" + flag-descs.usage(header) )
    Nothing
  }
}

fun main() {
  match (process-flags()) {
    Nothing -> ()
    Just(flags) -> build(flags.publish, flags.version || get-compiler-version())
  }
}

fun build( publish : bool, version : string ) : io () {
  val docsite  = "https://koka-lang.github.io/koka/doc/"
  val doclocal = "../koka-pages/doc".path

  val outspecx  = "out/spec".path;
  val outspec   = outspecx + "js-debug"
  val outstyles = outspec + "styles"
  val outscripts = outspec + "scripts"
  val outimages = outspec + "images"
  val specdir   = "doc/spec".path
  val docflags  = "--htmlcss=styles/madoko.css;styles/koka.css " + (if (publish) then "--htmlbases=" + docsite + " " else "")
  val cmd = "stack exec koka -- -c -l --target=js --outdir=" + outspecx.string + " -i" + specdir.string + " --html " + docflags
  println("compile standard libraries...")
  run-echo(cmd + " toc.kk")
  println("compile documentation inlined code...")
  run-echo(cmd + " kokaspec.kk.md spec.kk.md getstarted.kk.md overview.kk.md")
  println("patch kokaspec.md")
  file-replace-all(outspec + "kokaspec.md", ".kk.md", ".md");

  println("copy web files...")
  ensure-dir(outstyles)
  ensure-dir(outimages)
  ensure-dir(outscripts)
  copy-file-to-dir("doc/koka.css".path, outstyles)
  copy-file-to-dir("doc/logo/koka-logo.png".path, outimages)
  copy-file-to-dir(specdir + "koka.bib", outspec)
  copy-file-to-dir("lib/std/time/timescales.png".path, outspec)
  copy-directory(specdir + "styles", outstyles)
  copy-directory(specdir + "scripts", outscripts)

  val xmps = list-directory(outspec).
             filter(fn(p){ p.string.ends-with(".xmp.html").bool }).
             map(fn(p){ p.nodir.string })
  println("run madoko...")
  run-echo( "cd " + outspec.string + " && madoko --odir=. -v -mline-no:false -mlogo:false -mkokaversion:" + version
             + " " + xmps.join(" ") + " kokaspec.md" )
  copy-file-to-dir(outspec + "madoko.css", outstyles)
  if (publish) {
    println("copy files to output folder: " + doclocal.string)
    ensure-dir(doclocal)
    val htmls = list-directory(outspec).
                filter(fn(p){ p.extname == "html" && !p.string.ends-with(".xmp.html").bool })
    htmls.map fn(p){
      copy-file-to-dir(p,doclocal)
    }
    copy-directory(outstyles,doclocal + "styles")
  }
  println("done.")
}

fun file-replace-all( fname : path, pat : string, rep : string ) : io () {
  val content = read-text-file(fname)
  write-text-file( fname, content.replace-all(pat, rep) )
}

fun run-echo(cmd : string) : io () {
  // println("> " + cmd)
  val exit-code = run-system(cmd)
  if (exit-code==0) {
    println("ok")
  }
  else {
    println("> " + cmd)
    throw("error: " + exit-code.show )
  }
}